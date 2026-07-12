/*
 * "iOS Camera" async video source.
 *
 * A background thread connects to the companion app on the device — over
 * the LAN (phone IP entered in properties) or through usbmuxd (USB cable)
 * — and receives H.264/HEVC access units (Annex B), decoded via
 * libavcodec (GPU when available) into obs_source_output_video().
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
#include "web-control.h"
#include "lipsync.h"

#define WEB_CONTROL_PORT 9980

#define S_MODE "mode"
#define S_HOST "host"
#define S_LOW_LATENCY "low_latency"
#define S_HW_DECODE "hw_decode"
#define S_AUDIO_SOURCE "audio_sync_source"
#define S_AUDIO_SYNC "audio_sync_enabled"
#define S_AUDIO_LATENCY "audio_device_latency_ms"
#define S_AUTO_CALIBRATE "audio_auto_calibrate"
#define S_AUTO_VIDEO_DELAY "audio_auto_video_delay"
#define S_WEB_CONTROL "web_control"

/* Built-in OBS "Video Delay (Async)" filter. */
#define ASYNC_DELAY_FILTER_ID "async_delay_filter"
#define ASYNC_DELAY_FILTER_NAME "iOS Camera auto lip-sync delay"
#define MODE_DIAL "dial"
#define MODE_USB "usb"
#define T_(s) obs_module_text(s)

enum connection_mode {
	CONN_DIAL_NET, /* OBS dials the phone over the LAN */
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

	enum connection_mode conn_mode;
	char host[128];
	volatile bool hw_decode;

	/* Guarded by status_mutex (shared with the status string). */
	bool audio_sync;
	char audio_source[256];
	int64_t applied_audio_offset;
	int audio_device_latency_ms;
	bool auto_calibrate;
	bool auto_video_delay; /* delay video when the mic is the slower one */
	int64_t last_video_latency_ns; /* avg capture->decode, for the offset */

	/* Auto lip-sync cross-correlator. `lipsync` is fed by the network
	 * thread (reference) and the OBS audio thread (mic capture callback),
	 * so it has its own lock. `hooked_audio` is the source we registered
	 * the capture callback on (to unregister later). */
	struct lipsync *lipsync;
	pthread_mutex_t lipsync_mutex;
	obs_weak_source_t *hooked_audio;

	/* Recent accepted mic-delay measurements (network thread only), for
	 * median smoothing before applying an offset. */
	int64_t cal_hist[5];
	int cal_count;

	/* Remote-control commands queued for the device (JSON strings),
	 * pushed by the web control thread, drained by the stream thread.
	 * Guarded by status_mutex. */
#define CONTROL_QUEUE_MAX 16
	char *control_queue[CONTROL_QUEUE_MAX];
	int control_count;

	bool web_enabled;
	struct web_control *web;

	/* Latest camera-state JSON reported by the app (for remote UIs).
	 * Guarded by status_mutex. */
	char device_state[1024];

	pthread_mutex_t status_mutex;
	struct dstr status;
};

static enum connection_mode parse_mode(const char *mode)
{
	if (strcmp(mode, MODE_USB) == 0)
		return CONN_DIAL_USB;
	/* Includes configs saved by the removed phone-dials-OBS mode. */
	return CONN_DIAL_NET;
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

void ios_camera_enqueue_control(struct ios_camera_source *s, const char *json,
				size_t len)
{
	char *copy = bmalloc(len + 1);
	memcpy(copy, json, len);
	copy[len] = 0;

	pthread_mutex_lock(&s->status_mutex);
	if (s->control_count >= CONTROL_QUEUE_MAX) {
		/* Full: drop the oldest command. */
		bfree(s->control_queue[0]);
		memmove(&s->control_queue[0], &s->control_queue[1],
			sizeof(char *) * (CONTROL_QUEUE_MAX - 1));
		s->control_count--;
	}
	s->control_queue[s->control_count++] = copy;
	pthread_mutex_unlock(&s->status_mutex);
}

void ios_camera_copy_status(struct ios_camera_source *s, char *buf,
			    size_t size)
{
	pthread_mutex_lock(&s->status_mutex);
	snprintf(buf, size, "%s", s->status.array ? s->status.array : "");
	pthread_mutex_unlock(&s->status_mutex);
}

void ios_camera_copy_state(struct ios_camera_source *s, char *buf,
			   size_t size)
{
	pthread_mutex_lock(&s->status_mutex);
	snprintf(buf, size, "%s",
		 s->device_state[0] ? s->device_state : "{}");
	pthread_mutex_unlock(&s->status_mutex);
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
	bool hw_failed; /* GPU decode failed once: stay on software */
	enum AVCodecID codec_id;
	char name[128];
	struct latency_tracker lat;
};

/* ------------------------------------------------------------------ */
/* Auto lip-sync: tap the chosen mic source and cross-correlate it with the
 * phone's reference audio to measure the mic's true latency. */

static void mic_audio_callback(void *param, obs_source_t *source,
			       const struct audio_data *audio, bool muted)
{
	struct ios_camera_source *s = param;
	UNUSED_PARAMETER(source);

	if (muted || !audio->frames || !audio->data[0])
		return;

	uint32_t rate = audio_output_get_sample_rate(obs_get_audio());
	const float *mono = (const float *)audio->data[0];

	pthread_mutex_lock(&s->lipsync_mutex);
	if (s->lipsync)
		lipsync_add_mic(s->lipsync, mono, audio->frames, rate,
				audio->timestamp);
	pthread_mutex_unlock(&s->lipsync_mutex);
}

static void unhook_audio(struct ios_camera_source *s)
{
	if (!s->hooked_audio)
		return;
	obs_source_t *src = obs_weak_source_get_source(s->hooked_audio);
	if (src) {
		obs_source_remove_audio_capture_callback(
			src, mic_audio_callback, s);
		obs_source_release(src);
	}
	obs_weak_source_release(s->hooked_audio);
	s->hooked_audio = NULL;
}

static void hook_audio(struct ios_camera_source *s, const char *name)
{
	unhook_audio(s);
	if (!name || !name[0])
		return;
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return;
	obs_source_add_audio_capture_callback(src, mic_audio_callback, s);
	s->hooked_audio = obs_source_get_weak_source(src);
	obs_source_release(src);
}

#define CAL_MIN_CONFIDENCE 0.5
#define CAL_NS_MS 1000000LL

/*
 * Adds/updates/removes a private "Video Delay (Async)" filter on our own
 * source so the video can be delayed to line up with a slower microphone.
 * delay_ms <= 0 removes it. Safe to call from any thread (OBS guards the
 * filter list internally).
 */
static void set_video_delay(struct ios_camera_source *s, int delay_ms)
{
	obs_source_t *existing = obs_source_get_filter_by_name(
		s->source, ASYNC_DELAY_FILTER_NAME);

	if (delay_ms <= 0) {
		if (existing) {
			obs_source_filter_remove(s->source, existing);
			obs_source_release(existing);
		}
		return;
	}

	if (existing) {
		obs_data_t *settings = obs_source_get_settings(existing);
		if ((int)obs_data_get_int(settings, "delay_ms") != delay_ms) {
			obs_data_set_int(settings, "delay_ms", delay_ms);
			obs_source_update(existing, settings);
		}
		obs_data_release(settings);
		obs_source_release(existing);
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "delay_ms", delay_ms);
	obs_source_t *filter = obs_source_create_private(
		ASYNC_DELAY_FILTER_ID, ASYNC_DELAY_FILTER_NAME, settings);
	if (filter) {
		obs_source_filter_add(s->source, filter);
		obs_source_release(filter);
	}
	obs_data_release(settings);
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

/*
 * Cross-correlation result drives the offset. A single measurement is
 * noisy, so we gate on confidence and smooth over recent measurements
 * (median of the last few, only when they agree) before applying. Then
 * offset = videoLatency - micLatency; if the mic is actually slower than
 * the video path, audio delay can't help and we say so.
 */
static void apply_auto_calibrated(struct ios_camera_source *s,
				  int64_t video_latency_ns)
{
	char name[256];

	pthread_mutex_lock(&s->status_mutex);
	bool enabled = s->audio_sync && s->auto_calibrate;
	bool video_delay = s->auto_video_delay;
	snprintf(name, sizeof(name), "%s", s->audio_source);
	int64_t applied = s->applied_audio_offset;
	pthread_mutex_unlock(&s->status_mutex);

	if (!enabled || !name[0])
		return;

	int64_t mic_delay = 0;
	double conf = 0;
	pthread_mutex_lock(&s->lipsync_mutex);
	bool ok = s->lipsync &&
		  lipsync_estimate(s->lipsync, &mic_delay, &conf);
	pthread_mutex_unlock(&s->lipsync_mutex);

	if (!ok) {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: no reading (need the app's "
		     "'Auto lip-sync reference' on, plus speech/sound)");
		return;
	}
	if (conf < CAL_MIN_CONFIDENCE) {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: mic ~%d ms but low "
		     "confidence %.2f — ignoring (keep talking / raise mic "
		     "level)",
		     (int)(mic_delay / CAL_NS_MS), conf);
		return;
	}

	/* Accumulate confident readings; require agreement before acting. */
	int n = s->cal_count < 5 ? s->cal_count : 5;
	if (n == 5)
		memmove(&s->cal_hist[0], &s->cal_hist[1], sizeof(int64_t) * 4);
	s->cal_hist[n < 5 ? n : 4] = mic_delay;
	if (s->cal_count < 5)
		s->cal_count++;

	if (s->cal_count < 3) {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: mic ~%d ms (conf %.2f), "
		     "collecting %d/3…",
		     (int)(mic_delay / CAL_NS_MS), conf, s->cal_count);
		return;
	}

	int64_t sorted[5];
	memcpy(sorted, s->cal_hist, sizeof(int64_t) * (size_t)s->cal_count);
	qsort(sorted, (size_t)s->cal_count, sizeof(int64_t), cmp_i64);
	int64_t median = sorted[s->cal_count / 2];
	int64_t spread = sorted[s->cal_count - 1] - sorted[0];

	if (spread > 40 * CAL_NS_MS) {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: readings unstable "
		     "(spread %d ms) — holding",
		     (int)(spread / CAL_NS_MS));
		return;
	}

	int64_t offset = video_latency_ns - median;
	bool mic_slower = offset < 0;
	int video_delay_ms = mic_slower
				     ? (int)((median - video_latency_ns) /
					     CAL_NS_MS)
				     : 0;
	if (mic_slower)
		offset = 0;

	/* Delay the audio to match the video (mic faster) ... */
	int64_t diff = offset - applied;
	if (diff < 0)
		diff = -diff;
	if (diff >= 5 * CAL_NS_MS) {
		obs_source_t *audio = obs_get_source_by_name(name);
		if (audio) {
			obs_source_set_sync_offset(audio, offset);
			obs_source_release(audio);
			pthread_mutex_lock(&s->status_mutex);
			s->applied_audio_offset = offset;
			pthread_mutex_unlock(&s->status_mutex);
		}
	}

	/* ... or delay the video to match a slower mic, if enabled. */
	if (video_delay)
		set_video_delay(s, video_delay_ms);

	if (mic_slower && video_delay) {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: mic %d ms > video %d ms -> "
		     "delaying video by %d ms (conf %.2f)",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), video_delay_ms,
		     conf);
	} else if (mic_slower) {
		blog(LOG_WARNING,
		     "[ios-camera] auto lip-sync: mic latency %d ms exceeds "
		     "video %d ms — audio can't be delayed to match. Enable "
		     "'Auto video delay' to delay the video ~%d ms, or use a "
		     "lower-latency mic.",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), video_delay_ms);
	} else {
		blog(LOG_INFO,
		     "[ios-camera] auto lip-sync: mic %d ms, video %d ms -> "
		     "'%s' offset %d ms (conf %.2f)",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), name,
		     (int)(offset / CAL_NS_MS), conf);
	}
}

/*
 * Lip sync: keep the chosen OBS audio source's sync offset equal to the
 * measured camera latency, so audio is delayed to match the video.
 * 5 ms hysteresis avoids constantly rewriting a stable value.
 */
static void apply_audio_sync(struct ios_camera_source *s, int64_t latency_ns)
{
	char name[256];

	pthread_mutex_lock(&s->status_mutex);
	bool enabled = s->audio_sync;
	snprintf(name, sizeof(name), "%s", s->audio_source);
	int64_t applied = s->applied_audio_offset;
	int64_t device_latency_ns =
		(int64_t)s->audio_device_latency_ms * 1000000;
	pthread_mutex_unlock(&s->status_mutex);

	if (!enabled || !name[0])
		return;

	/* The audio device has its own capture latency; the delay we add
	 * only needs to cover the difference. */
	latency_ns -= device_latency_ns;
	if (latency_ns < 0)
		latency_ns = 0;

	int64_t diff = latency_ns - applied;
	if (diff < 0)
		diff = -diff;
	if (diff < 5000000)
		return;

	obs_source_t *audio = obs_get_source_by_name(name);
	if (!audio)
		return;
	obs_source_set_sync_offset(audio, latency_ns);
	obs_source_release(audio);

	pthread_mutex_lock(&s->status_mutex);
	s->applied_audio_offset = latency_ns;
	pthread_mutex_unlock(&s->status_mutex);

	blog(LOG_INFO, "[ios-camera] '%s' sync offset -> %d ms", name,
	     (int)(latency_ns / 1000000));
}

/* Forward queued remote-control commands to the device. */
static void control_tick(struct ios_camera_source *s, struct client_state *c)
{
	char *pending[CONTROL_QUEUE_MAX];
	int count;

	pthread_mutex_lock(&s->status_mutex);
	count = s->control_count;
	memcpy(pending, s->control_queue, sizeof(char *) * (size_t)count);
	s->control_count = 0;
	pthread_mutex_unlock(&s->status_mutex);

	for (int i = 0; i < count; i++) {
		size_t len = strlen(pending[i]);
		size_t total = OBSC_HEADER_SIZE + len;
		uint8_t *packet = bmalloc(total);
		obsc_build_header(packet, OBSC_PKT_CONTROL, 0, 0,
				  (uint32_t)len);
		memcpy(packet + OBSC_HEADER_SIZE, pending[i], len);

		size_t sent = 0;
		int spins = 0;
		while (sent < total && spins < 100) {
			int n = (int)send(c->sock,
					  (const char *)packet + sent,
					  (int)(total - sent), 0);
			if (n > 0) {
				sent += (size_t)n;
			} else {
				os_sleep_ms(1);
				spins++;
			}
		}

		bfree(packet);
		bfree(pending[i]);
	}
}

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

		/* Only steer audio sync from stable measurements: a
		 * congested link makes latency oscillate wildly, and
		 * chasing it would yank the mic delay around. */
		bool stable = (t->max_ns - t->min_ns) < 60000000ULL &&
			      t->offset_rtt < 50000000ULL;
		if (stable) {
			int64_t avg = (int64_t)(t->sum_ns / t->count);
			pthread_mutex_lock(&s->status_mutex);
			s->last_video_latency_ns = avg;
			bool auto_cal = s->auto_calibrate;
			pthread_mutex_unlock(&s->status_mutex);

			/* Auto-calibrate measures the mic latency directly;
			 * the manual path assumes a fixed device latency. */
			if (auto_cal)
				apply_auto_calibrated(s, avg);
			else
				apply_audio_sync(s, avg);
		}

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

	pthread_mutex_lock(&s->status_mutex);
	s->device_state[0] = 0;
	pthread_mutex_unlock(&s->status_mutex);
	set_status(s, "%s", T_("Status.Disconnected"));
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
	case OBSC_PKT_VIDEO_CONFIG: {
		blog(LOG_INFO, "[ios-camera] video config: %.*s",
		     (int)hdr->payload_size, (const char *)payload);

		char json[512] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);

		char codec[32] = {0};
		extract_json_string(json, "codec", codec, sizeof(codec));
		enum AVCodecID id = strcmp(codec, "hevc") == 0
					    ? AV_CODEC_ID_HEVC
					    : AV_CODEC_ID_H264;
		if (id != c->codec_id) {
			h264_decoder_destroy(c->decoder);
			c->decoder = NULL;
			c->codec_id = id;
		}
		break;
	}
	case OBSC_PKT_VIDEO:
		if (!c->decoder) {
			/* Join on a keyframe so the decoder starts clean. */
			if (!(hdr->flags & OBSC_FLAG_KEYFRAME))
				break;
			c->decoder = h264_decoder_create(
				c->codec_id != AV_CODEC_ID_NONE
					? c->codec_id
					: AV_CODEC_ID_H264,
				s->hw_decode && !c->hw_failed);
			if (!c->decoder)
				return false;
		}
		if (!h264_decoder_decode(c->decoder, s->source, payload,
					 hdr->payload_size, hdr->pts_ns)) {
			if (h264_decoder_is_hw(c->decoder)) {
				/* GPU path misbehaved: recreate in software
				 * for the rest of this connection. */
				blog(LOG_WARNING,
				     "[ios-camera] hardware decode error, "
				     "switching to software");
				c->hw_failed = true;
			} else {
				blog(LOG_WARNING,
				     "[ios-camera] decoder error, resetting");
			}
			h264_decoder_destroy(c->decoder);
			c->decoder = NULL;
			break;
		}
		latency_on_frame(&c->lat, hdr->pts_ns, os_gettime_ns());
		break;
	case OBSC_PKT_PING:
		break;
	case OBSC_PKT_STATE: {
		pthread_mutex_lock(&s->status_mutex);
		size_t n = hdr->payload_size < sizeof(s->device_state) - 1
				   ? hdr->payload_size
				   : sizeof(s->device_state) - 1;
		memcpy(s->device_state, payload, n);
		s->device_state[n] = 0;
		pthread_mutex_unlock(&s->status_mutex);
		break;
	}
	case OBSC_PKT_TIMESYNC_RESP:
		if (hdr->payload_size >= 8) {
			latency_on_timesync(&c->lat, obsc_read_u64(payload),
					    hdr->pts_ns, os_gettime_ns());
		}
		break;
	case OBSC_PKT_AUDIO: {
		pthread_mutex_lock(&s->status_mutex);
		bool want = s->auto_calibrate;
		pthread_mutex_unlock(&s->status_mutex);
		/* Need the phone->plugin clock offset to place the reference
		 * on our timeline; skip until timesync has locked. */
		if (!want || !c->lat.have_offset)
			break;
		int64_t plugin_time =
			(int64_t)hdr->pts_ns - c->lat.offset_ns;
		if (plugin_time < 0)
			break;
		/* Payload is S16LE PCM; header starts each packet 2-aligned. */
		const int16_t *pcm = (const int16_t *)payload;
		size_t samples = hdr->payload_size / 2;
		pthread_mutex_lock(&s->lipsync_mutex);
		if (s->lipsync)
			lipsync_add_reference(s->lipsync, pcm, samples,
					      OBSC_AUDIO_SAMPLE_RATE,
					      (uint64_t)plugin_time);
		pthread_mutex_unlock(&s->lipsync_mutex);
		break;
	}
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
 * Device-claim registry. All iOS Camera sources live in one process, so a
 * shared table lets exactly one source own a given device target (a host
 * IP, or "usb"). A second source aimed at the same device is refused
 * instead of fighting over the single connection the phone can serve —
 * which otherwise causes a rapid connect/disconnect flip-flop.
 */
#define MAX_CLAIMS 32
static pthread_mutex_t g_claim_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct device_claim {
	char key[192];
	void *owner;
} g_claims[MAX_CLAIMS];

/* Returns true if `owner` holds (or just acquired) the key. */
static bool device_claim(const char *key, void *owner)
{
	bool ok = false;
	int slot = -1;

	pthread_mutex_lock(&g_claim_mutex);
	for (int i = 0; i < MAX_CLAIMS; i++) {
		if (!g_claims[i].owner) {
			if (slot < 0)
				slot = i;
			continue;
		}
		if (strcmp(g_claims[i].key, key) == 0) {
			ok = g_claims[i].owner == owner; /* ours vs busy */
			pthread_mutex_unlock(&g_claim_mutex);
			return ok;
		}
	}
	if (slot >= 0) {
		snprintf(g_claims[slot].key, sizeof(g_claims[slot].key), "%s",
			 key);
		g_claims[slot].owner = owner;
		ok = true;
	}
	pthread_mutex_unlock(&g_claim_mutex);
	return ok;
}

static void device_release(void *owner)
{
	pthread_mutex_lock(&g_claim_mutex);
	for (int i = 0; i < MAX_CLAIMS; i++) {
		if (g_claims[i].owner == owner) {
			g_claims[i].owner = NULL;
			g_claims[i].key[0] = 0;
		}
	}
	pthread_mutex_unlock(&g_claim_mutex);
}

/*
 * Dial modes: the app on the device listens; we connect to it — over the
 * LAN (host set in properties) or through usbmuxd. Retry every couple of
 * seconds until the app is reachable.
 */
static void sleep_ms_interruptible(struct ios_camera_source *s, int total_ms)
{
	for (int i = 0; i < total_ms / 100 && !s->stop; i++)
		os_sleep_ms(100);
}

static void dial_loop(struct ios_camera_source *s)
{
	bool usb = s->conn_mode == CONN_DIAL_USB;
	/* The device we currently own, e.g. "net:192.168.1.5" or "usb:3".
	 * Held across reconnects; a second source aimed at the same device
	 * is refused so they don't fight over the one stream the phone can
	 * serve. Different devices (different IPs, or different USB phones)
	 * each get their own claim and run independently. */
	char claimed_key[192] = {0};
	bool claimed = false;

	while (!s->stop) {
		socket_t sock = OBSC_INVALID_SOCKET;
		long usb_id = -1;

		if (usb) {
			long ids[16];
			int n = usbmux_list_devices(ids, 16);

			/* Drop our claim if that phone was unplugged. */
			if (claimed) {
				long mine = strtol(claimed_key + 4, NULL, 10);
				bool present = false;
				for (int i = 0; i < n; i++)
					if (ids[i] == mine)
						present = true;
				if (!present) {
					device_release(s);
					claimed = false;
					claimed_key[0] = 0;
				}
			}
			/* Claim the first attached phone no other source owns. */
			if (!claimed) {
				for (int i = 0; i < n; i++) {
					char k[32];
					snprintf(k, sizeof(k), "usb:%ld",
						 ids[i]);
					if (device_claim(k, s)) {
						claimed = true;
						snprintf(claimed_key,
							 sizeof(claimed_key),
							 "%s", k);
						break;
					}
				}
			}
			if (!claimed) {
				set_status(s, "%s",
					   n > 0 ? T_("Status.DeviceBusy")
						 : T_("Status.WaitingUSB"));
				sleep_ms_interruptible(s, 1000);
				continue;
			}
			usb_id = strtol(claimed_key + 4, NULL, 10);
			set_status(s, "%s", T_("Status.WaitingUSB"));
			sock = usbmux_connect_device(usb_id, OBSC_USB_PORT);
		} else if (!s->host[0]) {
			set_status(s, "%s", T_("Status.NoHost"));
			sleep_ms_interruptible(s, 2000);
			continue;
		} else {
			if (!claimed) {
				char k[192];
				snprintf(k, sizeof(k), "net:%s", s->host);
				if (!device_claim(k, s)) {
					set_status(s, "%s",
						   T_("Status.DeviceBusy"));
					sleep_ms_interruptible(s, 1000);
					continue;
				}
				claimed = true;
				snprintf(claimed_key, sizeof(claimed_key), "%s",
					 k);
			}
			set_status(s, "%s %s", T_("Status.Dialing"), s->host);
			sock = tcp_dial(s->host, OBSC_USB_PORT);
		}

		if (sock == OBSC_INVALID_SOCKET) {
			sleep_ms_interruptible(s, 2000);
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
			control_tick(s, &client);
			if (ret == 0)
				continue;
			if (!client_read(s, &client))
				break;
		}

		blog(LOG_INFO, "[ios-camera] device connection ended");
		client_disconnect(s, &client);
	}

	if (claimed)
		device_release(s);
}

static void *server_thread(void *data)
{
	struct ios_camera_source *s = data;

	os_set_thread_name("ios-camera-server");

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
	enum connection_mode mode =
		parse_mode(obs_data_get_string(settings, S_MODE));
	const char *host = obs_data_get_string(settings, S_HOST);

	/* Unbuffered async video: render the newest frame immediately
	 * instead of letting OBS smooth timestamps with a frame queue. */
	obs_source_set_async_unbuffered(
		s->source, obs_data_get_bool(settings, S_LOW_LATENCY));
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);

	pthread_mutex_lock(&s->status_mutex);
	bool was_syncing = s->audio_sync && s->audio_source[0];
	int64_t applied = s->applied_audio_offset;
	char old_source[256];
	snprintf(old_source, sizeof(old_source), "%s", s->audio_source);

	s->audio_sync = obs_data_get_bool(settings, S_AUDIO_SYNC);
	s->audio_device_latency_ms =
		(int)obs_data_get_int(settings, S_AUDIO_LATENCY);
	s->auto_calibrate = obs_data_get_bool(settings, S_AUTO_CALIBRATE);
	s->auto_video_delay =
		obs_data_get_bool(settings, S_AUTO_VIDEO_DELAY);
	snprintf(s->audio_source, sizeof(s->audio_source), "%s",
		 obs_data_get_string(settings, S_AUDIO_SOURCE));
	bool still_syncing = s->audio_sync &&
			     strcmp(old_source, s->audio_source) == 0;
	if (!still_syncing)
		s->applied_audio_offset = 0;
	bool want_hook =
		s->audio_sync && s->auto_calibrate && s->audio_source[0];
	bool want_video_delay = want_hook && s->auto_video_delay;
	char hook_name[256];
	snprintf(hook_name, sizeof(hook_name), "%s", s->audio_source);
	pthread_mutex_unlock(&s->status_mutex);

	/* Manage the mic capture hook for auto-calibration. */
	bool was_hooked = s->hooked_audio != NULL;
	bool source_changed = strcmp(old_source, hook_name) != 0;
	if (want_hook && (!was_hooked || source_changed)) {
		hook_audio(s, hook_name);
		pthread_mutex_lock(&s->lipsync_mutex);
		if (s->lipsync)
			lipsync_reset(s->lipsync);
		pthread_mutex_unlock(&s->lipsync_mutex);
		s->cal_count = 0;
	} else if (!want_hook && was_hooked) {
		unhook_audio(s);
	}

	/* Remove our video-delay filter when the feature is off; the apply
	 * loop re-adds/updates it when needed. */
	if (!want_video_delay)
		set_video_delay(s, 0);

	bool web = obs_data_get_bool(settings, S_WEB_CONTROL);
	if (web && !s->web)
		s->web = web_control_start(s, WEB_CONTROL_PORT);
	else if (!web && s->web) {
		web_control_stop(s->web);
		s->web = NULL;
	}

	/* Turned off (or retargeted): undo the offset we applied. */
	if (was_syncing && !still_syncing && applied != 0) {
		obs_source_t *audio = obs_get_source_by_name(old_source);
		if (audio) {
			obs_source_set_sync_offset(audio, 0);
			obs_source_release(audio);
		}
	}

	if (mode != s->conn_mode || strcmp(host, s->host) != 0) {
		stop_thread(s);
		s->conn_mode = mode;
		snprintf(s->host, sizeof(s->host), "%s", host);
		start_thread(s);
	}
}

static void *ios_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct ios_camera_source *s = bzalloc(sizeof(*s));
	s->source = source;

	/* Init locks/state before anything that might use them (the web
	 * thread reads status; the audio callback reads lipsync). */
	pthread_mutex_init(&s->status_mutex, NULL);
	pthread_mutex_init(&s->lipsync_mutex, NULL);
	dstr_init(&s->status);
	s->lipsync = lipsync_create();

	s->conn_mode = parse_mode(obs_data_get_string(settings, S_MODE));
	snprintf(s->host, sizeof(s->host), "%s",
		 obs_data_get_string(settings, S_HOST));
	obs_source_set_async_unbuffered(
		source, obs_data_get_bool(settings, S_LOW_LATENCY));
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	s->audio_sync = obs_data_get_bool(settings, S_AUDIO_SYNC);
	s->audio_device_latency_ms =
		(int)obs_data_get_int(settings, S_AUDIO_LATENCY);
	s->auto_calibrate = obs_data_get_bool(settings, S_AUTO_CALIBRATE);
	s->auto_video_delay =
		obs_data_get_bool(settings, S_AUTO_VIDEO_DELAY);
	snprintf(s->audio_source, sizeof(s->audio_source), "%s",
		 obs_data_get_string(settings, S_AUDIO_SOURCE));

	if (s->audio_sync && s->auto_calibrate && s->audio_source[0])
		hook_audio(s, s->audio_source);
	if (obs_data_get_bool(settings, S_WEB_CONTROL))
		s->web = web_control_start(s, WEB_CONTROL_PORT);

	start_thread(s);
	return s;
}

static void ios_camera_destroy(void *data)
{
	struct ios_camera_source *s = data;

	web_control_stop(s->web);
	unhook_audio(s);
	stop_thread(s);
	for (int i = 0; i < s->control_count; i++)
		bfree(s->control_queue[i]);
	lipsync_destroy(s->lipsync);
	dstr_free(&s->status);
	pthread_mutex_destroy(&s->lipsync_mutex);
	pthread_mutex_destroy(&s->status_mutex);
	bfree(s);
}

static void ios_camera_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_MODE, MODE_DIAL);
	obs_data_set_default_string(settings, S_HOST, "");
	obs_data_set_default_bool(settings, S_LOW_LATENCY, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_bool(settings, S_AUDIO_SYNC, false);
	obs_data_set_default_string(settings, S_AUDIO_SOURCE, "");
	obs_data_set_default_int(settings, S_AUDIO_LATENCY, 0);
	obs_data_set_default_bool(settings, S_AUTO_CALIBRATE, false);
	obs_data_set_default_bool(settings, S_AUTO_VIDEO_DELAY, false);
	obs_data_set_default_bool(settings, S_WEB_CONTROL, true);
}

static bool add_audio_source(void *data, obs_source_t *source)
{
	obs_property_t *list = data;
	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)
		obs_property_list_add_string(list,
					     obs_source_get_name(source),
					     obs_source_get_name(source));
	return true;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *property,
			  obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	enum connection_mode mode =
		parse_mode(obs_data_get_string(settings, S_MODE));
	obs_property_set_visible(obs_properties_get(props, S_HOST),
				 mode == CONN_DIAL_NET);
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
	obs_property_set_modified_callback(mode, mode_modified);

	obs_properties_add_text(props, S_HOST, T_("Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, S_LOW_LATENCY, T_("LowLatency"));
	obs_properties_add_bool(props, S_HW_DECODE, T_("HwDecode"));

	obs_property_t *audio_list = obs_properties_add_list(
		props, S_AUDIO_SOURCE, T_("AudioSource"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, T_("AudioSource.None"), "");
	obs_enum_sources(add_audio_source, audio_list);
	obs_properties_add_bool(props, S_AUDIO_SYNC, T_("AudioSync"));
	obs_properties_add_bool(props, S_AUTO_CALIBRATE, T_("AutoCalibrate"));
	obs_properties_add_bool(props, S_AUTO_VIDEO_DELAY,
				T_("AutoVideoDelay"));
	obs_property_t *audio_lat = obs_properties_add_int(
		props, S_AUDIO_LATENCY, T_("AudioLatency"), 0, 500, 1);
	obs_property_int_set_suffix(audio_lat, " ms");

	obs_properties_add_bool(props, S_WEB_CONTROL, T_("WebControl"));

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
