/*
 * "iOS Camera" async video source.
 *
 * Runs a small TCP server on a background thread. The iOS companion app
 * connects and streams H.264 access units (Annex B) which are decoded with
 * libavcodec and handed to OBS via obs_source_output_video(). A UDP
 * responder on port+1 answers LAN discovery probes from the app.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <string.h>
#include <stdio.h>

#include "net-compat.h"
#include "protocol.h"
#include "h264-decoder.h"
#include "usbmux.h"

#define S_PORT "port"
#define S_MODE "mode"
#define S_HOST "host"
#define S_LOW_LATENCY "low_latency"
#define MODE_NETWORK "network"
#define MODE_DIAL "dial"
#define MODE_USB "usb"
#define T_(s) obs_module_text(s)

enum connection_mode {
	CONN_LISTEN,   /* phone dials OBS (legacy) */
	CONN_DIAL_NET, /* OBS dials the phone over the LAN (recommended) */
	CONN_DIAL_USB, /* OBS dials the phone through usbmuxd */
};

#define RECV_CHUNK 65536

struct recv_buf {
	uint8_t *data;
	size_t len;
	size_t cap;
};

struct ios_camera_source {
	obs_source_t *source;

	pthread_t thread;
	bool thread_active;
	volatile bool stop;

	int port;
	enum connection_mode conn_mode;
	char host[128];

	pthread_mutex_t status_mutex;
	struct dstr status;
};

static enum connection_mode parse_mode(const char *mode)
{
	if (strcmp(mode, MODE_DIAL) == 0)
		return CONN_DIAL_NET;
	if (strcmp(mode, MODE_USB) == 0)
		return CONN_DIAL_USB;
	return CONN_LISTEN;
}

/* ------------------------------------------------------------------ */

static void set_status(struct ios_camera_source *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	pthread_mutex_lock(&s->status_mutex);
	dstr_vprintf(&s->status, fmt, args);
	pthread_mutex_unlock(&s->status_mutex);

	va_end(args);
}

static void recv_buf_free(struct recv_buf *b)
{
	bfree(b->data);
	memset(b, 0, sizeof(*b));
}

static void recv_buf_append(struct recv_buf *b, const uint8_t *data,
			    size_t size)
{
	if (b->len + size > b->cap) {
		size_t new_cap = b->cap ? b->cap : RECV_CHUNK;
		while (new_cap < b->len + size)
			new_cap *= 2;
		b->data = brealloc(b->data, new_cap);
		b->cap = new_cap;
	}
	memcpy(b->data + b->len, data, size);
	b->len += size;
}

static void recv_buf_consume(struct recv_buf *b, size_t size)
{
	if (size >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + size, b->len - size);
	b->len -= size;
}

/* ------------------------------------------------------------------ */

static socket_t open_tcp_listener(int port)
{
	socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == OBSC_INVALID_SOCKET)
		return sock;

	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
		   sizeof(yes));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(sock, 1) != 0) {
		net_close(sock);
		return OBSC_INVALID_SOCKET;
	}

	net_set_nonblocking(sock);
	return sock;
}

static socket_t open_discovery_socket(int port)
{
	socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == OBSC_INVALID_SOCKET)
		return sock;

	int yes = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
		   sizeof(yes));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)(port + OBSC_DISCOVERY_PORT_OFFSET));

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		net_close(sock);
		return OBSC_INVALID_SOCKET;
	}

	net_set_nonblocking(sock);
	return sock;
}

static void answer_discovery(socket_t sock, int tcp_port)
{
	char probe[64];
	struct sockaddr_in from = {0};
	socklen_t from_len = sizeof(from);

	int n = (int)recvfrom(sock, probe, sizeof(probe) - 1, 0,
			      (struct sockaddr *)&from, &from_len);
	if (n <= 0)
		return;
	probe[n] = 0;

	if (strncmp(probe, OBSC_DISCOVER_REQUEST,
		    strlen(OBSC_DISCOVER_REQUEST)) != 0)
		return;

	char host[128] = "OBS";
	gethostname(host, sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;

	char reply[192];
	snprintf(reply, sizeof(reply), OBSC_DISCOVER_REPLY_PREFIX "%d:%s",
		 tcp_port, host);

	sendto(sock, reply, (int)strlen(reply), 0, (struct sockaddr *)&from,
	       from_len);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Latency measurement: NTP-style clock offset between the phone's
 * capture clock and this machine, then per-frame capture→decode time. */

struct latency_tracker {
	bool have_offset;
	int64_t offset_ns;   /* phone_clock - plugin_clock */
	uint64_t offset_rtt; /* RTT of the sample behind offset_ns */
	uint64_t offset_time;

	uint64_t sum_ns;
	uint64_t count;
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t window_start;
	uint64_t last_sync_send;
};

static void latency_on_timesync(struct latency_tracker *t, uint64_t t1,
				uint64_t t2, uint64_t t3)
{
	if (t3 <= t1)
		return;

	uint64_t rtt = t3 - t1;
	int64_t offset = (int64_t)t2 - (int64_t)((t1 + t3) / 2);

	/* Prefer low-RTT samples (least asymmetry error), but refresh
	 * periodically anyway so clock drift can't accumulate. */
	bool stale = t3 - t->offset_time > 10000000000ULL;
	if (!t->have_offset || rtt <= t->offset_rtt || stale) {
		t->have_offset = true;
		t->offset_ns = offset;
		t->offset_rtt = rtt;
		t->offset_time = t3;
	}
}

static void latency_on_frame(struct latency_tracker *t, uint64_t pts_phone_ns,
			     uint64_t now_ns)
{
	if (!t->have_offset)
		return;

	int64_t captured_local =
		(int64_t)pts_phone_ns - t->offset_ns;
	int64_t lat = (int64_t)now_ns - captured_local;
	if (lat < 0)
		lat = 0;

	uint64_t l = (uint64_t)lat;
	if (!t->count || l < t->min_ns)
		t->min_ns = l;
	if (!t->count || l > t->max_ns)
		t->max_ns = l;
	t->sum_ns += l;
	t->count++;
	if (!t->window_start)
		t->window_start = now_ns;
}

struct client_state {
	socket_t sock;
	struct recv_buf buf;
	struct h264_decoder *decoder;
	char name[128];
	struct latency_tracker lat;
};

/* Once a second, ask the phone for a clock sample; every five, report. */
static void latency_tick(struct ios_camera_source *s, struct client_state *c)
{
	uint64_t now = os_gettime_ns();
	struct latency_tracker *t = &c->lat;

	if (now - t->last_sync_send > 1000000000ULL) {
		t->last_sync_send = now;
		uint8_t hdr[OBSC_HEADER_SIZE];
		obsc_build_header(hdr, OBSC_PKT_TIMESYNC_REQ, 0, now, 0);
		/* Best effort; a 20-byte send into an open socket either
		 * fully succeeds or the connection is toast anyway. */
		send(c->sock, (const char *)hdr, OBSC_HEADER_SIZE, 0);
	}

	if (t->count && t->window_start &&
	    now - t->window_start > 5000000000ULL) {
		unsigned avg_ms = (unsigned)(t->sum_ns / t->count / 1000000);
		unsigned min_ms = (unsigned)(t->min_ns / 1000000);
		unsigned max_ms = (unsigned)(t->max_ns / 1000000);
		unsigned rtt_ms = (unsigned)(t->offset_rtt / 1000000);

		blog(LOG_INFO,
		     "[ios-camera] capture->decode latency: avg %u ms "
		     "(min %u / max %u), link rtt %u ms, %u frames",
		     avg_ms, min_ms, max_ms, rtt_ms, (unsigned)t->count);
		set_status(s, "%s %s — ~%u ms", T_("Status.Connected"),
			   c->name[0] ? c->name : "iOS device", avg_ms);

		t->sum_ns = 0;
		t->count = 0;
		t->window_start = now;
	}
}

static void client_disconnect(struct ios_camera_source *s,
			      struct client_state *c)
{
	if (c->sock != OBSC_INVALID_SOCKET) {
		net_close(c->sock);
		c->sock = OBSC_INVALID_SOCKET;
	}
	recv_buf_free(&c->buf);
	h264_decoder_destroy(c->decoder);
	c->decoder = NULL;
	c->name[0] = 0;

	/* Clear the last frame so the source goes blank when the phone
	 * disconnects instead of freezing on a stale image. */
	obs_source_output_video(s->source, NULL);
	set_status(s, "%s (%d)", T_("Status.Listening"), s->port);
}

static void extract_json_string(const char *json, const char *key, char *out,
				size_t out_size)
{
	/* Tiny best-effort extraction of "key":"value" — enough for the
	 * HELLO payload without pulling in a JSON dependency. */
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);

	const char *p = strstr(json, pattern);
	if (!p)
		return;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '"')
		return;
	p++;

	size_t i = 0;
	while (*p && *p != '"' && i + 1 < out_size)
		out[i++] = *p++;
	out[i] = 0;
}

static bool handle_packet(struct ios_camera_source *s, struct client_state *c,
			  const struct obsc_header *hdr,
			  const uint8_t *payload)
{
	switch (hdr->type) {
	case OBSC_PKT_HELLO: {
		char json[512] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);
		extract_json_string(json, "name", c->name, sizeof(c->name));
		blog(LOG_INFO, "[ios-camera] client connected: %s",
		     c->name[0] ? c->name : "(unnamed)");
		set_status(s, "%s %s", T_("Status.Connected"),
			   c->name[0] ? c->name : "iOS device");
		break;
	}
	case OBSC_PKT_VIDEO_CONFIG:
		blog(LOG_INFO, "[ios-camera] video config: %.*s",
		     (int)hdr->payload_size, (const char *)payload);
		break;
	case OBSC_PKT_VIDEO:
		if (!c->decoder) {
			/* Join on a keyframe so the decoder starts clean. */
			if (!(hdr->flags & OBSC_FLAG_KEYFRAME))
				break;
			c->decoder = h264_decoder_create();
			if (!c->decoder)
				return false;
		}
		if (!h264_decoder_decode(c->decoder, s->source, payload,
					 hdr->payload_size, hdr->pts_ns)) {
			blog(LOG_WARNING,
			     "[ios-camera] decoder error, resetting");
			h264_decoder_destroy(c->decoder);
			c->decoder = NULL;
			break;
		}
		latency_on_frame(&c->lat, hdr->pts_ns, os_gettime_ns());
		break;
	case OBSC_PKT_PING:
		break;
	case OBSC_PKT_TIMESYNC_RESP:
		if (hdr->payload_size >= 8) {
			latency_on_timesync(&c->lat, obsc_read_u64(payload),
					    hdr->pts_ns, os_gettime_ns());
		}
		break;
	default:
		blog(LOG_WARNING, "[ios-camera] unknown packet type %d",
		     hdr->type);
		break;
	}
	return true;
}

static bool client_read(struct ios_camera_source *s, struct client_state *c)
{
	uint8_t chunk[RECV_CHUNK];
	int n = (int)recv(c->sock, (char *)chunk, sizeof(chunk), 0);
	if (n == 0) {
		blog(LOG_INFO, "[ios-camera] connection closed by device");
		return false;
	}
	if (n < 0) {
		blog(LOG_INFO, "[ios-camera] recv error %d", net_last_error());
		return false;
	}

	recv_buf_append(&c->buf, chunk, (size_t)n);

	while (c->buf.len >= OBSC_HEADER_SIZE) {
		struct obsc_header hdr;
		if (!obsc_parse_header(c->buf.data, &hdr)) {
			blog(LOG_WARNING,
			     "[ios-camera] bad packet header, dropping client");
			return false;
		}

		size_t total = OBSC_HEADER_SIZE + hdr.payload_size;
		if (c->buf.len < total)
			break;

		if (!handle_packet(s, c, &hdr,
				   c->buf.data + OBSC_HEADER_SIZE))
			return false;

		recv_buf_consume(&c->buf, total);
	}

	return true;
}

/* TCP connect with a 3-second timeout; returns a non-blocking socket. */
static socket_t tcp_dial(const char *host, uint16_t port)
{
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

	struct addrinfo hints = {0};
	struct addrinfo *res = NULL;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return OBSC_INVALID_SOCKET;

	socket_t s = socket(res->ai_family, res->ai_socktype,
			    res->ai_protocol);
	if (s == OBSC_INVALID_SOCKET) {
		freeaddrinfo(res);
		return OBSC_INVALID_SOCKET;
	}

	net_set_nonblocking(s);
	int ret = connect(s, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);

	if (ret != 0) {
#ifdef _WIN32
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			goto fail;
#else
		if (errno != EINPROGRESS)
			goto fail;
#endif
		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(s, &wfds);
		struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
		if (select((int)s + 1, NULL, &wfds, NULL, &tv) != 1)
			goto fail;

		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
		if (err != 0)
			goto fail;
	}

	int yes = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes,
		   sizeof(yes));
	return s;

fail:
	net_close(s);
	return OBSC_INVALID_SOCKET;
}

/*
 * Dial modes: the app on the device listens; we connect to it — over the
 * LAN (host set in properties) or through usbmuxd. Retry every couple of
 * seconds until the app is reachable.
 */
static void dial_loop(struct ios_camera_source *s)
{
	bool usb = s->conn_mode == CONN_DIAL_USB;

	while (!s->stop) {
		socket_t sock = OBSC_INVALID_SOCKET;

		if (usb) {
			set_status(s, "%s", T_("Status.WaitingUSB"));
			sock = usbmux_connect_first(OBSC_USB_PORT);
		} else if (!s->host[0]) {
			set_status(s, "%s", T_("Status.NoHost"));
		} else {
			set_status(s, "%s %s", T_("Status.Dialing"), s->host);
			sock = tcp_dial(s->host, OBSC_USB_PORT);
		}

		if (sock == OBSC_INVALID_SOCKET) {
			for (int i = 0; i < 20 && !s->stop; i++)
				os_sleep_ms(100);
			continue;
		}

		blog(LOG_INFO, "[ios-camera] connected to device (%s)",
		     usb ? "USB" : "network");
		set_status(s, "%s", T_("Status.Connected"));

		struct client_state client = {.sock = sock};

		while (!s->stop) {
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(sock, &fds);
			struct timeval tv = {.tv_sec = 0,
					     .tv_usec = 200 * 1000};
			int ret = select((int)sock + 1, &fds, NULL, NULL, &tv);
			if (ret < 0)
				break;
			latency_tick(s, &client);
			if (ret == 0)
				continue;
			if (!client_read(s, &client))
				break;
		}

		blog(LOG_INFO, "[ios-camera] device connection ended");
		client_disconnect(s, &client);
	}
}

static void network_loop(struct ios_camera_source *s)
{
	socket_t listener = open_tcp_listener(s->port);
	socket_t discovery = open_discovery_socket(s->port);
	struct client_state client = {.sock = OBSC_INVALID_SOCKET};

	if (listener == OBSC_INVALID_SOCKET) {
		blog(LOG_ERROR, "[ios-camera] failed to listen on port %d",
		     s->port);
		set_status(s, "%s (%d)", T_("Status.PortError"), s->port);
	} else {
		blog(LOG_INFO, "[ios-camera] listening on port %d", s->port);
		set_status(s, "%s (%d)", T_("Status.Listening"), s->port);
	}

	while (!s->stop && listener != OBSC_INVALID_SOCKET) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(listener, &fds);
		socket_t max_fd = listener;

		if (discovery != OBSC_INVALID_SOCKET) {
			FD_SET(discovery, &fds);
			if (discovery > max_fd)
				max_fd = discovery;
		}
		if (client.sock != OBSC_INVALID_SOCKET) {
			FD_SET(client.sock, &fds);
			if (client.sock > max_fd)
				max_fd = client.sock;
		}

		struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
		int ret = select((int)max_fd + 1, &fds, NULL, NULL, &tv);
		if (ret < 0)
			break;

		if (client.sock != OBSC_INVALID_SOCKET)
			latency_tick(s, &client);

		if (ret == 0)
			continue;

		if (discovery != OBSC_INVALID_SOCKET &&
		    FD_ISSET(discovery, &fds))
			answer_discovery(discovery, s->port);

		if (FD_ISSET(listener, &fds)) {
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			socket_t accepted = accept(
				listener, (struct sockaddr *)&from, &from_len);
			if (accepted != OBSC_INVALID_SOCKET) {
				/* Newest connection wins; drop the old one. */
				if (client.sock != OBSC_INVALID_SOCKET) {
					blog(LOG_INFO,
					     "[ios-camera] replacing existing connection");
					client_disconnect(s, &client);
				}
				net_set_nonblocking(accepted);
				int yes = 1;
				setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
					   (const char *)&yes, sizeof(yes));
				client.sock = accepted;
				set_status(s, "%s", T_("Status.Connected"));
			}
		}

		if (client.sock != OBSC_INVALID_SOCKET &&
		    FD_ISSET(client.sock, &fds)) {
			if (!client_read(s, &client)) {
				blog(LOG_INFO,
				     "[ios-camera] client disconnected");
				client_disconnect(s, &client);
			}
		}
	}

	if (client.sock != OBSC_INVALID_SOCKET)
		client_disconnect(s, &client);
	if (listener != OBSC_INVALID_SOCKET)
		net_close(listener);
	if (discovery != OBSC_INVALID_SOCKET)
		net_close(discovery);
}

static void *server_thread(void *data)
{
	struct ios_camera_source *s = data;

	os_set_thread_name("ios-camera-server");

	if (s->conn_mode == CONN_LISTEN)
		network_loop(s);
	else
		dial_loop(s);

	return NULL;
}

/* ------------------------------------------------------------------ */

static void stop_thread(struct ios_camera_source *s)
{
	if (!s->thread_active)
		return;
	s->stop = true;
	pthread_join(s->thread, NULL);
	s->thread_active = false;
	s->stop = false;
}

static void start_thread(struct ios_camera_source *s)
{
	if (s->thread_active)
		return;
	if (pthread_create(&s->thread, NULL, server_thread, s) == 0)
		s->thread_active = true;
	else
		blog(LOG_ERROR, "[ios-camera] failed to start server thread");
}

static const char *ios_camera_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("IOSCameraSource");
}

static void ios_camera_update(void *data, obs_data_t *settings)
{
	struct ios_camera_source *s = data;
	int port = (int)obs_data_get_int(settings, S_PORT);
	enum connection_mode mode =
		parse_mode(obs_data_get_string(settings, S_MODE));
	const char *host = obs_data_get_string(settings, S_HOST);

	/* Unbuffered async video: render the newest frame immediately
	 * instead of letting OBS smooth timestamps with a frame queue. */
	obs_source_set_async_unbuffered(
		s->source, obs_data_get_bool(settings, S_LOW_LATENCY));

	if (port != s->port || mode != s->conn_mode ||
	    strcmp(host, s->host) != 0) {
		stop_thread(s);
		s->port = port;
		s->conn_mode = mode;
		snprintf(s->host, sizeof(s->host), "%s", host);
		start_thread(s);
	}
}

static void *ios_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct ios_camera_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->port = (int)obs_data_get_int(settings, S_PORT);
	s->conn_mode = parse_mode(obs_data_get_string(settings, S_MODE));
	snprintf(s->host, sizeof(s->host), "%s",
		 obs_data_get_string(settings, S_HOST));
	obs_source_set_async_unbuffered(
		source, obs_data_get_bool(settings, S_LOW_LATENCY));

	pthread_mutex_init(&s->status_mutex, NULL);
	dstr_init(&s->status);

	start_thread(s);
	return s;
}

static void ios_camera_destroy(void *data)
{
	struct ios_camera_source *s = data;

	stop_thread(s);
	dstr_free(&s->status);
	pthread_mutex_destroy(&s->status_mutex);
	bfree(s);
}

static void ios_camera_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_PORT, OBSC_DEFAULT_PORT);
	obs_data_set_default_string(settings, S_MODE, MODE_DIAL);
	obs_data_set_default_string(settings, S_HOST, "");
	obs_data_set_default_bool(settings, S_LOW_LATENCY, true);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *property,
			  obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	enum connection_mode mode =
		parse_mode(obs_data_get_string(settings, S_MODE));
	obs_property_set_visible(obs_properties_get(props, S_HOST),
				 mode == CONN_DIAL_NET);
	obs_property_set_visible(obs_properties_get(props, S_PORT),
				 mode == CONN_LISTEN);
	return true;
}

static obs_properties_t *ios_camera_get_properties(void *data)
{
	struct ios_camera_source *s = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(
		props, S_MODE, T_("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, T_("Mode.Dial"), MODE_DIAL);
	obs_property_list_add_string(mode, T_("Mode.USB"), MODE_USB);
	obs_property_list_add_string(mode, T_("Mode.Network"), MODE_NETWORK);
	obs_property_set_modified_callback(mode, mode_modified);

	obs_properties_add_text(props, S_HOST, T_("Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, S_PORT, T_("Port"), 1024, 65535, 1);
	obs_properties_add_bool(props, S_LOW_LATENCY, T_("LowLatency"));

	obs_property_t *status = obs_properties_add_text(
		props, "status_info", T_("Status"), OBS_TEXT_INFO);
	if (s) {
		pthread_mutex_lock(&s->status_mutex);
		obs_property_set_long_description(
			status, s->status.array ? s->status.array : "");
		pthread_mutex_unlock(&s->status_mutex);
	}

	obs_properties_add_text(props, "help_info", T_("HelpText"),
				OBS_TEXT_INFO);

	return props;
}

struct obs_source_info ios_camera_source_info = {
	.id = "ios_camera_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ios_camera_get_name,
	.create = ios_camera_create,
	.destroy = ios_camera_destroy,
	.update = ios_camera_update,
	.get_defaults = ios_camera_get_defaults,
	.get_properties = ios_camera_get_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
