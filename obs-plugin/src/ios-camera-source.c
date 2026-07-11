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
#define MODE_NETWORK "network"
#define MODE_USB "usb"
#define T_(s) obs_module_text(s)

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
	bool usb_mode;

	pthread_mutex_t status_mutex;
	struct dstr status;
};

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

struct client_state {
	socket_t sock;
	struct recv_buf buf;
	struct h264_decoder *decoder;
	char name[128];
};

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
		}
		break;
	case OBSC_PKT_PING:
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

/*
 * USB mode: the app on the device listens; we dial it through usbmuxd.
 * Retry every couple of seconds until a device with the app appears.
 */
static void usb_loop(struct ios_camera_source *s)
{
	while (!s->stop) {
		set_status(s, "%s", T_("Status.WaitingUSB"));

		socket_t sock = usbmux_connect_first(OBSC_USB_PORT);
		if (sock == OBSC_INVALID_SOCKET) {
			for (int i = 0; i < 20 && !s->stop; i++)
				os_sleep_ms(100);
			continue;
		}

		blog(LOG_INFO, "[ios-camera] connected to device over USB");
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
			if (ret == 0)
				continue;
			if (!client_read(s, &client))
				break;
		}

		blog(LOG_INFO, "[ios-camera] USB connection ended");
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

	if (s->usb_mode)
		usb_loop(s);
	else
		network_loop(s);

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
	bool usb = strcmp(obs_data_get_string(settings, S_MODE), MODE_USB) == 0;

	if (port != s->port || usb != s->usb_mode) {
		stop_thread(s);
		s->port = port;
		s->usb_mode = usb;
		start_thread(s);
	}
}

static void *ios_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct ios_camera_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->port = (int)obs_data_get_int(settings, S_PORT);
	s->usb_mode =
		strcmp(obs_data_get_string(settings, S_MODE), MODE_USB) == 0;

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
	obs_data_set_default_string(settings, S_MODE, MODE_NETWORK);
}

static obs_properties_t *ios_camera_get_properties(void *data)
{
	struct ios_camera_source *s = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(
		props, S_MODE, T_("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, T_("Mode.Network"), MODE_NETWORK);
	obs_property_list_add_string(mode, T_("Mode.USB"), MODE_USB);

	obs_properties_add_int(props, S_PORT, T_("Port"), 1024, 65535, 1);

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
