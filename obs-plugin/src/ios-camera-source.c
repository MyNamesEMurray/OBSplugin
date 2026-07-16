/*
 * "LensLink Camera" async video source.
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
#include <stdlib.h>
#include <time.h>

#include "net-compat.h"
#include "protocol.h"
#include "h264-decoder.h"
#include "usbmux.h"
#include "web-control.h"
#include "lipsync.h"
#include "plugin-settings.h"
#include "gpu-frame.h"
#include "pipeline-bench.h"

#include <libavutil/frame.h>

/* Snapshot of the plugin-wide pipeline choice, taken once at module load
 * (it decides how the source types are registered). */
static bool g_gpu_pipeline_mode = false;

struct ios_camera_source;
static void gpu_pipeline_sink(void *ud, struct AVFrame *frame);
static void gpu_pipeline_clear(struct ios_camera_source *s);
static void gpu_pipeline_free(struct ios_camera_source *s);
#include "mdns.h"
#include "health.h"

/* Bonjour service the app advertises while its listener is up. */
#define LENSLINK_MDNS_SERVICE "_lenslink._tcp.local"


#define S_MODE "mode"
#define S_HOST "host"
#define S_USB_DEVICE "usb_device"
#define S_LOW_LATENCY "low_latency"
#define S_HW_DECODE "hw_decode"
#define S_AUDIO_SOURCE "audio_sync_source"
#define S_AUDIO_SYNC "audio_sync_enabled"
#define S_AUDIO_LATENCY "audio_device_latency_ms"
#define S_AUTO_CALIBRATE "audio_auto_calibrate"
#define S_AUTO_VIDEO_DELAY "audio_auto_video_delay"
#define S_DEACTIVATE_HIDDEN "deactivate_when_hidden"
#define S_AUTO_START "auto_start"

/* Built-in OBS "Video Delay (Async)" filter. */
#define ASYNC_DELAY_FILTER_ID "async_delay_filter"
#define ASYNC_DELAY_FILTER_NAME "LensLink auto lip-sync delay"
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
	char usb_device[64]; /* pinned UDID for USB mode; "" = auto-assign */
	volatile bool hw_decode;
	volatile bool diagnostics; /* verbose pipeline logging to the OBS log */
	volatile bool dump_stream; /* write received video to a file on disk */

	/* "Deactivate when hidden": when enabled and the source isn't shown
	 * on any display (program, preview, or projector), the dial loop
	 * disconnects and releases the device claim — the phone stops
	 * encoding (battery) and becomes usable by another LensLink source.
	 * `showing` is maintained by the show/hide callbacks. */
	volatile bool deactivate_hidden;
	volatile bool showing;

	/* Remote start: when the app connects in standby (open but idle,
	 * "Remote start from OBS" enabled), send it a start_stream command
	 * automatically. `auto_start_armed` implements once-per-appearance:
	 * it's set when the source is created, after the phone has been
	 * unreachable for a couple of attempts (app closed/backgrounded), or
	 * when we stop the camera ourselves on hide — and consumed by the
	 * auto-start. So a manual stop on the phone doesn't bounce straight
	 * back into streaming. armed/dial_failures are touched only by the
	 * dial-loop thread. */
	volatile bool auto_start;
	bool auto_start_armed;
	int dial_failures;

	/* Which registered source type this instance is: "LensLink Screen"
	 * (true) or "LensLink Camera" (false). Set once at create from the
	 * source id, never changes. Screen sources skip all camera-only
	 * machinery (web panel, lip sync); what the *phone* actually sends is
	 * `is_screen` below — a mismatch is warned about in the status. */
	bool is_screen_source;

	/* Guarded by status_mutex (shared with the status string). */
	bool audio_sync;
	char audio_source[256];
	int64_t applied_audio_offset;
	int audio_device_latency_ms;
	bool auto_calibrate;
	bool auto_video_delay; /* delay video when the mic is the slower one */
	/* set_video_delay mirrored a delay onto the source's sync offset:
	 * clear the offset only when we set it, never a user's manual value
	 * (set_video_delay(0) runs on every settings update). */
	bool sync_offset_owned;
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

	/* Health mirrors for the frontend UI (the live counters belong to
	 * the dial thread's client_state; these are 1 Hz copies). Guarded
	 * by status_mutex. stat_connected doubles as the web panel's
	 * "live controls make sense" signal (set at HELLO, cleared on
	 * disconnect; the 1 Hz tick re-asserts it). */
	bool stat_connected;
	uint64_t stat_frames;
	uint64_t stat_bytes;
	char stat_device[64];

	/* The current connection is a screen mirror (no camera controls).
	 * Guarded by status_mutex; lets the web panel hide dead controls. */
	bool is_screen;

	/* The connected app is idle in standby, waiting for start_stream.
	 * Guarded by status_mutex; lets the web panel offer a Start button. */
	bool standby;

	pthread_mutex_t status_mutex;
	struct dstr status;

	/* --- GPU (beta) pipeline state; used only in sync-source mode --- */
	/* Newest decoded frame from the dial thread, and the frame the
	 * render currently displays. Guarded by frame_mutex. */
	pthread_mutex_t frame_mutex;
	AVFrame *pending_frame;
	AVFrame *current_frame;
	uint32_t frame_width, frame_height; /* last decoded dims */
	/* Graphics-thread-only interop + CPU-upload fallback state. */
	struct gpu_frame_ctx *gpu;
	struct gpu_frame_map_result gpu_map;
	bool gpu_mapped;
	gs_texture_t *cpu_tex[3];
	uint32_t cpu_tex_w, cpu_tex_h;
	int cpu_tex_fmt; /* AVPixelFormat the fallback textures match */

	/* Port the running web server bound (0 = not running); lets a
	 * settings change restart it on the new port. */
	int web_port;
};

/* Starts/stops/rebinds the web panel to match the plugin-wide settings.
 * Main/UI thread only (create/update/the settings dialog). */
static void apply_web_settings(struct ios_camera_source *s)
{
	if (s->is_screen_source)
		return;
	bool want = lenslink_settings_web_enabled();
	int port = lenslink_settings_web_port();

	if (s->web && (!want || s->web_port != port)) {
		web_control_stop(s->web);
		s->web = NULL;
		s->web_port = 0;
	}
	if (want && !s->web) {
		s->web = web_control_start(s, (uint16_t)port);
		s->web_port = s->web ? port : 0;
	}
}

bool ios_camera_is_screen(struct ios_camera_source *s)
{
	pthread_mutex_lock(&s->status_mutex);
	bool v = s->is_screen;
	pthread_mutex_unlock(&s->status_mutex);
	return v;
}

bool ios_camera_is_standby(struct ios_camera_source *s)
{
	pthread_mutex_lock(&s->status_mutex);
	bool v = s->standby;
	pthread_mutex_unlock(&s->status_mutex);
	return v;
}

/* ------------------------------------------------------------------ */
/* Health registry: which sources exist, for the frontend UI. Sources
 * register at create and unregister first thing at destroy, so a snapshot
 * can never touch a freeing source (enum holds the same mutex). */

#define MAX_HEALTH_SOURCES 16
static pthread_mutex_t g_health_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ios_camera_source *g_health_sources[MAX_HEALTH_SOURCES];

static void health_register(struct ios_camera_source *s)
{
	pthread_mutex_lock(&g_health_mutex);
	for (int i = 0; i < MAX_HEALTH_SOURCES; i++) {
		if (!g_health_sources[i]) {
			g_health_sources[i] = s;
			break;
		}
	}
	pthread_mutex_unlock(&g_health_mutex);
}

static void health_unregister(struct ios_camera_source *s)
{
	pthread_mutex_lock(&g_health_mutex);
	for (int i = 0; i < MAX_HEALTH_SOURCES; i++) {
		if (g_health_sources[i] == s)
			g_health_sources[i] = NULL;
	}
	pthread_mutex_unlock(&g_health_mutex);
}

/* The plugin-wide settings changed (Tools -> LensLink Settings): push
 * them into every live source. UI thread. */
void ios_camera_apply_global_settings(void)
{
	pthread_mutex_lock(&g_health_mutex);
	for (int i = 0; i < MAX_HEALTH_SOURCES; i++) {
		struct ios_camera_source *s = g_health_sources[i];
		if (!s)
			continue;
		s->diagnostics = lenslink_settings_diagnostics();
		s->dump_stream = lenslink_settings_dump_stream();
		apply_web_settings(s);
	}
	pthread_mutex_unlock(&g_health_mutex);
}

size_t lenslink_health_enum(struct lenslink_health *out, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&g_health_mutex);
	for (int i = 0; i < MAX_HEALTH_SOURCES && n < max; i++) {
		struct ios_camera_source *s = g_health_sources[i];
		if (!s)
			continue;
		struct lenslink_health *h = &out[n++];
		const char *name = obs_source_get_name(s->source);
		snprintf(h->source_name, sizeof(h->source_name), "%s",
			 name ? name : "");
		h->is_screen = s->is_screen_source;
		pthread_mutex_lock(&s->status_mutex);
		h->connected = s->stat_connected;
		h->standby = s->standby;
		h->frames = s->stat_frames;
		h->bytes = s->stat_bytes;
		h->latency_ms = (int)(s->last_video_latency_ns / 1000000);
		snprintf(h->device, sizeof(h->device), "%s", s->stat_device);
		snprintf(h->status, sizeof(h->status), "%s",
			 s->status.array ? s->status.array : "");
		pthread_mutex_unlock(&s->status_mutex);
	}
	pthread_mutex_unlock(&g_health_mutex);
	return n;
}

bool ios_camera_is_connected(struct ios_camera_source *s)
{
	pthread_mutex_lock(&s->status_mutex);
	bool v = s->stat_connected;
	pthread_mutex_unlock(&s->status_mutex);
	return v;
}

bool ios_camera_auto_start(struct ios_camera_source *s)
{
	return s->auto_start;
}

/* Web-panel toggle for the auto-start property: persist it through the
 * source's settings so the properties checkbox and the panel stay one
 * value. obs_source_update is safe from the web thread; our update()
 * only restarts the dial thread when connection settings change. */
void ios_camera_set_auto_start(struct ios_camera_source *s, bool on)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_bool(settings, S_AUTO_START, on);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
}

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

/* Makes room for `size` more bytes after the current contents. */
static void recv_buf_reserve(struct recv_buf *b, size_t size)
{
	if (b->len + size > b->cap) {
		size_t new_cap = b->cap ? b->cap : RECV_CHUNK;
		while (new_cap < b->len + size)
			new_cap *= 2;
		b->data = brealloc(b->data, new_cap);
		b->cap = new_cap;
	}
}

/* Discards the first `size` bytes. Called once per recv cycle (not per
 * packet) so the memmove of the tail isn't O(packets x bytes). A large
 * keyframe can balloon the buffer to many MB; shrink once it drains so a
 * spike doesn't pin that memory for the rest of the connection. */
static void recv_buf_consume(struct recv_buf *b, size_t size)
{
	if (size >= b->len)
		b->len = 0;
	else {
		memmove(b->data, b->data + size, b->len - size);
		b->len -= size;
	}

	if (b->cap > 2 * RECV_CHUNK && b->len < RECV_CHUNK) {
		size_t new_cap = 2 * RECV_CHUNK;
		uint8_t *shrunk = bmalloc(new_cap);
		memcpy(shrunk, b->data, b->len);
		bfree(b->data);
		b->data = shrunk;
		b->cap = new_cap;
	}
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

/* Outbound bytes the non-blocking socket wouldn't take yet. A packet must
 * never be abandoned part-way: a truncated frame desyncs the app's parser
 * until reconnect. If the peer stalls long enough to accumulate this much
 * un-sent control/timesync data, the connection is dead — drop it. */
#define SEND_BUF_MAX (256 * 1024)

struct send_buf {
	uint8_t *data;
	size_t len;
	size_t cap;
	bool failed; /* hard send error: connection must be dropped */
};

struct client_state {
	socket_t sock;
	struct recv_buf buf;
	struct send_buf out;
	struct h264_decoder *decoder;
	/* Next hardware-API priority slot to try. Each hardware decoder
	 * that errors or silently emits nothing advances this by one, so
	 * the connection walks the platform's whole list (e.g. D3D11VA →
	 * DXVA2) before settling on software — a driver rejecting a stream
	 * on one API frequently decodes it fine on the next. */
	int hw_retry;
	/* video_packets at the current decoder's creation: each decoder in
	 * the fallback walk gets its own no-output grace window. */
	uint64_t packets_at_decoder;
	bool is_screen; /* screen mirror (plays system audio) vs camera */
	bool standby;   /* app is idle, waiting for a start_stream command */
	uint64_t next_decoder_attempt; /* cooldown after create failure */
	enum AVCodecID codec_id;
	char name[128];
	struct latency_tracker lat;

	/* Diagnostics for the screen-mirror pipeline (all counters cumulative
	 * over the connection). Touched only by the server thread. */
	uint64_t connected_ns;   /* HELLO arrival, for time-to-first-frame */
	uint64_t video_packets;
	uint64_t video_bytes;
	uint64_t keyframes_seen;
	uint64_t frames_output;  /* frames the decoder actually produced */
	uint64_t decode_errors;
	uint64_t audio_packets;
	uint64_t audio_frames;
	/* Type-9 lip-sync reference packets this connection. Zero a few
	 * seconds into a live stream means the app's reference toggle is off
	 * for this stream — the signal to clear stale calibration output. */
	uint64_t ref_audio_packets;
	bool ref_absent_handled; /* the clear ran once this connection */
	int audio_peak; /* loudest |sample| since the last heartbeat */
	uint64_t last_diag_ns;   /* last heartbeat emission */
	uint64_t last_stats_ns;  /* last health-mirror update */
	uint64_t first_frame_ns; /* 0 until the first decoded frame */
	bool no_output_warned;   /* one-shot "keyframe but nothing decoded" */
	bool sps_logged;         /* one-shot SPS profile/level log */
	bool wrong_kind; /* stream kind doesn't match this source type */

	/* Stream-dump diagnostic: the received Annex B bytes, written
	 * verbatim so the exact live stream can be replayed through the
	 * ffmpeg CLI when hardware decode misbehaves. Starts at a keyframe
	 * (so the file joins clean, like the decoder does). */
	FILE *dump_file;
	bool dump_tried; /* fopen failed once — don't retry per packet */
};

static void send_buf_free(struct send_buf *b)
{
	bfree(b->data);
	memset(b, 0, sizeof(*b));
}

/* Flushes queued bytes as far as the socket allows. Sets `failed` on a
 * hard error; EWOULDBLOCK just leaves the remainder queued. */
static void client_flush(struct client_state *c)
{
	struct send_buf *b = &c->out;
	while (b->len > 0 && !b->failed) {
		int n = (int)send(c->sock, (const char *)b->data,
				  (int)b->len, 0);
		if (n > 0) {
			memmove(b->data, b->data + n, b->len - (size_t)n);
			b->len -= (size_t)n;
		} else if (n < 0 && net_would_block()) {
			return;
		} else {
			b->failed = true;
		}
	}
}

/* Queues (and opportunistically sends) a complete packet. Never sends
 * part of `data` without queuing the rest, so framing stays intact. */
static void client_send(struct client_state *c, const void *data, size_t len)
{
	struct send_buf *b = &c->out;
	const uint8_t *p = data;

	if (b->failed)
		return;

	/* Nothing queued: push as much as the socket takes right now. */
	if (b->len == 0) {
		while (len > 0) {
			int n = (int)send(c->sock, (const char *)p, (int)len,
					  0);
			if (n > 0) {
				p += n;
				len -= (size_t)n;
			} else if (n < 0 && net_would_block()) {
				break;
			} else {
				b->failed = true;
				return;
			}
		}
		if (len == 0)
			return;
	}

	if (b->len + len > SEND_BUF_MAX) {
		blog(LOG_WARNING,
		     "[lenslink] peer not draining control channel, "
		     "dropping connection");
		b->failed = true;
		return;
	}
	if (b->len + len > b->cap) {
		size_t new_cap = b->cap ? b->cap : 4096;
		while (new_cap < b->len + len)
			new_cap *= 2;
		b->data = brealloc(b->data, new_cap);
		b->cap = new_cap;
	}
	memcpy(b->data + b->len, p, len);
	b->len += len;
}

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

	/* The async-delay filter shifts ONLY video — a source's own audio
	 * (the phone mic) bypasses the filter chain and would end up leading
	 * the delayed video by exactly delay_ms. Mirror the delay onto the
	 * source's sync offset (audio-only, a no-op while the source carries
	 * no audio) so the pair moves together. In practice the mic can't be
	 * source audio while calibration runs (one mic, one role), but a
	 * calibration-applied delay outlives the reference role, so the
	 * mirror still matters. Clear ONLY an offset we set: this runs with
	 * 0 on every settings update, and it must not wipe a sync offset the
	 * user typed into Advanced Audio Properties. */
	if (delay_ms > 0) {
		obs_source_set_sync_offset(s->source,
					   (int64_t)delay_ms * 1000000);
		s->sync_offset_owned = true;
	} else if (s->sync_offset_owned) {
		obs_source_set_sync_offset(s->source, 0);
		s->sync_offset_owned = false;
	}

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

	/* Hold the mutex only for a snapshot: the correlation is ~1.8M
	 * multiply-adds, and the OBS audio callback blocks on this lock. */
	int64_t mic_delay = 0;
	double conf = 0;
	pthread_mutex_lock(&s->lipsync_mutex);
	struct lipsync *snap = s->lipsync ? lipsync_clone(s->lipsync) : NULL;
	pthread_mutex_unlock(&s->lipsync_mutex);

	bool ok = snap && lipsync_estimate(snap, &mic_delay, &conf);
	lipsync_destroy(snap);

	if (!ok) {
		blog(LOG_INFO,
		     "[lenslink] auto lip-sync: no reading (need the app's "
		     "'Auto lip-sync reference' on, plus speech/sound)");
		return;
	}
	if (conf < CAL_MIN_CONFIDENCE) {
		blog(LOG_INFO,
		     "[lenslink] auto lip-sync: mic ~%d ms but low "
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
		     "[lenslink] auto lip-sync: mic ~%d ms (conf %.2f), "
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
		     "[lenslink] auto lip-sync: readings unstable "
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
		     "[lenslink] auto lip-sync: mic %d ms > video %d ms -> "
		     "delaying video by %d ms (conf %.2f)",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), video_delay_ms,
		     conf);
	} else if (mic_slower) {
		blog(LOG_WARNING,
		     "[lenslink] auto lip-sync: mic latency %d ms exceeds "
		     "video %d ms — audio can't be delayed to match. Enable "
		     "'Auto video delay' to delay the video ~%d ms, or use a "
		     "lower-latency mic.",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), video_delay_ms);
	} else {
		blog(LOG_INFO,
		     "[lenslink] auto lip-sync: mic %d ms, video %d ms -> "
		     "'%s' offset %d ms (conf %.2f)",
		     (int)(median / CAL_NS_MS),
		     (int)(video_latency_ns / CAL_NS_MS), name,
		     (int)(offset / CAL_NS_MS), conf);
	}
}

/*
 * The app's "Auto lip-sync reference" toggle can be turned off between
 * streams, and nothing tells the plugin — its own auto-calibrate settings
 * stay on, so a previously applied video delay and audio offset would
 * outlive the calibration that produced them, silently skewing A/V.
 * Called when a stream has been live for a few seconds with auto-calibrate
 * enabled but zero reference packets: the reference is off for this
 * stream, so clear everything calibration applied. (Settings-side disable
 * is handled separately in update().)
 */
static void lipsync_reference_absent(struct ios_camera_source *s)
{
	char name[256];
	pthread_mutex_lock(&s->status_mutex);
	bool calibrating = s->audio_sync && s->auto_calibrate;
	int64_t applied = s->applied_audio_offset;
	snprintf(name, sizeof(name), "%s", s->audio_source);
	pthread_mutex_unlock(&s->status_mutex);
	if (!calibrating)
		return;

	bool cleared = s->sync_offset_owned;
	set_video_delay(s, 0);

	if (applied != 0 && name[0]) {
		obs_source_t *audio = obs_get_source_by_name(name);
		if (audio) {
			obs_source_set_sync_offset(audio, 0);
			obs_source_release(audio);
		}
		pthread_mutex_lock(&s->status_mutex);
		s->applied_audio_offset = 0;
		pthread_mutex_unlock(&s->status_mutex);
		cleared = true;
	}

	if (cleared)
		blog(LOG_INFO,
		     "[lenslink] auto lip-sync: the app sent no reference "
		     "this stream (its reference option is off) — cleared "
		     "the applied video delay and audio offset");
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

	blog(LOG_INFO, "[lenslink] '%s' sync offset -> %d ms", name,
	     (int)(latency_ns / 1000000));
}

/* Sends a control command (JSON) to the device. Best-effort; receivers
 * ignore commands they don't understand, so new ones stay compatible. */
static void send_control_cmd(struct client_state *c, const char *json)
{
	size_t len = strlen(json);
	uint8_t hdr[OBSC_HEADER_SIZE];
	obsc_build_header(hdr, OBSC_PKT_CONTROL, 0, 0, (uint32_t)len);
	client_send(c, hdr, OBSC_HEADER_SIZE);
	client_send(c, json, len);
}

/* Asks the device to emit a fresh keyframe immediately. The screen
 * extension honours it, the camera app ignores unknown commands. Used to
 * avoid a multi-second black gap after swapping the decoder to software —
 * the software decoder can only (re)start on a keyframe. */
static void request_keyframe(struct client_state *c)
{
	send_control_cmd(c, "{\"cmd\":\"keyframe\"}");
}

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

		client_send(c, packet, total);

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
		client_send(c, hdr, OBSC_HEADER_SIZE);
	}

	if (t->count && t->window_start &&
	    now - t->window_start > 5000000000ULL) {
		unsigned avg_ms = (unsigned)(t->sum_ns / t->count / 1000000);
		unsigned min_ms = (unsigned)(t->min_ns / 1000000);
		unsigned max_ms = (unsigned)(t->max_ns / 1000000);
		unsigned rtt_ms = (unsigned)(t->offset_rtt / 1000000);

		blog(LOG_INFO,
		     "[lenslink] capture->decode latency: avg %u ms "
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

/* Periodic pipeline heartbeat to the OBS log (every ~2s while connected).
 * The counts moving — or not — between beats is what pinpoints where a
 * screen mirror stalls: no packets = nothing sent; packets but dec=0 =
 * decode problem; frames but nothing on screen = an OBS transform issue. */
static void diag_tick(struct ios_camera_source *s, struct client_state *c)
{
	if (!s->diagnostics || c->connected_ns == 0)
		return;

	uint64_t now = os_gettime_ns();
	if (c->last_diag_ns && now - c->last_diag_ns < 2000000000ULL)
		return;
	c->last_diag_ns = now;

	uint64_t elapsed_ms = (now - c->connected_ns) / 1000000ULL;
	double secs = (double)elapsed_ms / 1000.0;
	double mbps = secs > 0.0 ? (double)c->video_bytes * 8.0 / secs / 1e6
				 : 0.0;

	char frame_info[96];
	if (c->frames_output > 0) {
		int w = 0, h = 0;
		const char *fmt = "?";
		h264_decoder_last_frame(c->decoder, &w, &h, &fmt);
		snprintf(frame_info, sizeof(frame_info), "%dx%d %s %s", w, h,
			 fmt,
			 (c->decoder && h264_decoder_is_hw(c->decoder)) ? "GPU"
									: "CPU");
	} else {
		snprintf(frame_info, sizeof(frame_info), "NO FRAMES DECODED");
	}

	blog(LOG_INFO,
	     "[lenslink][diag] %s t=%llus | vid pkt=%llu kf=%llu dec=%llu "
	     "err=%llu %.1f Mbps | aud pkt=%llu fr=%llu peak=%d%% | %s",
	     c->is_screen ? "screen" : "camera",
	     (unsigned long long)(elapsed_ms / 1000ULL),
	     (unsigned long long)c->video_packets,
	     (unsigned long long)c->keyframes_seen,
	     (unsigned long long)c->frames_output,
	     (unsigned long long)c->decode_errors, mbps,
	     (unsigned long long)c->audio_packets,
	     (unsigned long long)c->audio_frames,
	     c->audio_peak * 100 / 32767, frame_info);
	c->audio_peak = 0;
}

/* Mirrors the dial thread's live counters into the source (~1 Hz) for
 * the frontend UI's health readouts. */
static void stats_tick(struct ios_camera_source *s, struct client_state *c)
{
	uint64_t now = os_gettime_ns();
	if (now - c->last_stats_ns < 1000000000ULL)
		return;
	c->last_stats_ns = now;

	/* Piggyback the pipeline benchmark logger on this 1 Hz tick (it
	 * rate-limits itself and no-ops when the setting is off). */
	lenslink_bench_maybe_log();

	pthread_mutex_lock(&s->status_mutex);
	s->stat_connected = true;
	s->stat_frames = c->frames_output;
	s->stat_bytes = c->video_bytes;
	snprintf(s->stat_device, sizeof(s->stat_device), "%.63s", c->name);
	pthread_mutex_unlock(&s->status_mutex);
}

static void dump_close(struct client_state *c)
{
	if (c->dump_file) {
		fclose(c->dump_file);
		c->dump_file = NULL;
		blog(LOG_INFO, "[lenslink] stream dump closed");
	}
	c->dump_tried = false;
}

/* Opens the stream-dump file in the plugin's config directory. Named with
 * the wall-clock time so successive dumps never overwrite each other, and
 * with the codec's conventional extension so `ffmpeg -i` autodetects it. */
static void dump_open(struct client_state *c)
{
	c->dump_tried = true;

	char *dir = obs_module_config_path(NULL);
	if (!dir)
		return;
	os_mkdirs(dir);

	struct dstr path = {0};
	dstr_printf(&path, "%s/lenslink-dump-%lld.%s", dir,
		    (long long)time(NULL),
		    c->codec_id == AV_CODEC_ID_HEVC ? "hevc" : "h264");
	bfree(dir);

	c->dump_file = os_fopen(path.array, "wb");
	if (c->dump_file)
		blog(LOG_INFO,
		     "[lenslink] dumping received %s stream to %s",
		     c->codec_id == AV_CODEC_ID_HEVC ? "HEVC" : "H.264",
		     path.array);
	else
		blog(LOG_WARNING, "[lenslink] stream dump: cannot open %s",
		     path.array);
	dstr_free(&path);
}

static void client_disconnect(struct ios_camera_source *s,
			      struct client_state *c)
{
	dump_close(c);
	if (c->sock != OBSC_INVALID_SOCKET) {
		net_close(c->sock);
		c->sock = OBSC_INVALID_SOCKET;
	}
	recv_buf_free(&c->buf);
	send_buf_free(&c->out);
	h264_decoder_destroy(c->decoder);
	c->decoder = NULL;
	c->name[0] = 0;

	/* Clear the last frame so the source goes blank when the phone
	 * disconnects instead of freezing on a stale image. */
	obs_source_output_video(s->source, NULL);
	gpu_pipeline_clear(s);

	pthread_mutex_lock(&s->status_mutex);
	s->device_state[0] = 0;
	s->is_screen = false;
	s->standby = false;
	s->stat_connected = false;
	s->stat_device[0] = 0;
	pthread_mutex_unlock(&s->status_mutex);

	/* A kind-mismatch rejection needs its actionable status to survive
	 * the disconnect — "Disconnected" would hide what to fix. */
	if (c->wrong_kind)
		set_status(s, "%s",
			   s->is_screen_source ? T_("Status.CameraOnScreen")
					       : T_("Status.ScreenOnCamera"));
	else
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

/* True only for a bare-boolean "key": true (same best-effort spirit as
 * extract_json_string). Absent key or any other value reads as false. */
static bool extract_json_bool(const char *json, const char *key)
{
	char pattern[64];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);

	const char *p = strstr(json, pattern);
	if (!p)
		return false;
	p = strchr(p + strlen(pattern), ':');
	if (!p)
		return false;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	return strncmp(p, "true", 4) == 0;
}

/* Logs the H.264 stream's SPS profile/level once per decode stream — the
 * first thing to know when a GPU driver won't decode a stream software
 * handles fine (drivers gate on the advertised caps, not the content). */
static void log_h264_sps(const uint8_t *data, size_t size)
{
	for (size_t i = 0; i + 4 < size; i++) {
		if (data[i] != 0 || data[i + 1] != 0)
			continue;
		size_t nal;
		if (data[i + 2] == 1)
			nal = i + 3;
		else if (data[i + 2] == 0 && data[i + 3] == 1)
			nal = i + 4;
		else
			continue;
		if (nal + 3 >= size)
			return;
		if ((data[nal] & 0x1F) != 7) /* SPS */
			continue;
		blog(LOG_INFO,
		     "[lenslink] H.264 SPS: profile_idc=%u "
		     "constraint_flags=0x%02x level_idc=%u",
		     data[nal + 1], data[nal + 2], data[nal + 3]);
		return;
	}
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
		char kind[16] = {0};
		extract_json_string(json, "kind", kind, sizeof(kind));
		c->is_screen = strcmp(kind, "screen") == 0;
		c->standby = !c->is_screen && extract_json_bool(json, "standby");
		c->connected_ns = os_gettime_ns();
		pthread_mutex_lock(&s->status_mutex);
		s->is_screen = c->is_screen;
		s->standby = c->standby;
		s->stat_connected = true;
		pthread_mutex_unlock(&s->status_mutex);
		blog(LOG_INFO, "[lenslink] client connected: %s (%s%s)",
		     c->name[0] ? c->name : "(unnamed)",
		     c->is_screen ? "screen" : "camera",
		     c->standby ? ", standby" : "");
		/* The phone picks what it streams, not this source; each source
		 * type accepts only its own kind. Rejecting here (rather than
		 * displaying with a warning) keeps a Screen source from ever
		 * showing a camera and vice versa — the status says what to do
		 * instead, and the dial loop backs off before retrying. */
		if (c->is_screen != s->is_screen_source) {
			c->wrong_kind = true;
			blog(LOG_INFO,
			     "[lenslink] rejecting %s stream on a %s source",
			     c->is_screen ? "screen" : "camera",
			     s->is_screen_source ? "screen" : "camera");
			return false;
		}
		if (c->standby) {
			/* Remote start: the app is open but idle. Kick the
			 * camera off ourselves if enabled and armed (armed =
			 * the app just became reachable, not "the user just
			 * pressed Stop"); otherwise say how to start it. */
			if (s->auto_start && s->auto_start_armed) {
				s->auto_start_armed = false;
				blog(LOG_INFO,
				     "[lenslink] app is idle — starting the "
				     "camera remotely");
				send_control_cmd(c,
						 "{\"cmd\":\"start_stream\"}");
				set_status(s, "%s",
					   T_("Status.StartingCamera"));
			} else {
				set_status(s, "%s", T_("Status.Standby"));
			}
			break;
		}
		set_status(s, "%s %s", T_("Status.Connected"),
			   c->name[0] ? c->name : "iOS device");
		break;
	}
	case OBSC_PKT_VIDEO_CONFIG: {
		/* Clamp: payload_size can legally be up to 16 MB, and a
		 * malformed packet must not dump that into the log. */
		int log_len = hdr->payload_size < 256
				      ? (int)hdr->payload_size
				      : 256;
		blog(LOG_INFO, "[lenslink] video config: %.*s", log_len,
		     (const char *)payload);

		char json[512] = {0};
		size_t n = hdr->payload_size < sizeof(json) - 1
				   ? hdr->payload_size
				   : sizeof(json) - 1;
		memcpy(json, payload, n);

		char codec[32] = {0};
		extract_json_string(json, "codec", codec, sizeof(codec));
		char kind[16] = {0};
		extract_json_string(json, "kind", kind, sizeof(kind));
		if (kind[0])
			c->is_screen = strcmp(kind, "screen") == 0;

		/* Video config means the stream is (re)starting — a standby
		 * connection has left standby. (A remote start also re-sends
		 * HELLO, but don't rely on it.) */
		if (c->standby) {
			c->standby = false;
			pthread_mutex_lock(&s->status_mutex);
			s->standby = false;
			pthread_mutex_unlock(&s->status_mutex);
			set_status(s, "%s %s", T_("Status.Connected"),
				   c->name[0] ? c->name : "iOS device");
		}
		enum AVCodecID id = strcmp(codec, "hevc") == 0
					    ? AV_CODEC_ID_HEVC
					    : AV_CODEC_ID_H264;
		if (id != c->codec_id) {
			h264_decoder_destroy(c->decoder);
			c->decoder = NULL;
			c->codec_id = id;
			/* A codec switch starts a NEW decode stream, so the
			 * failover machinery must re-arm with it. The silent
			 * no-output watchdog only fires while frames_output
			 * is 0 — a stale count from the previous codec kept
			 * it disarmed, so a hardware decoder that accepted
			 * packets but emitted nothing froze the source
			 * forever (works on a fresh connection, froze on a
			 * mid-stream HEVC→H.264 switch). hw_retry resets
			 * too: it described the OLD codec's hardware path;
			 * the new codec deserves its own walk of the
			 * hardware list, with the watchdog and the error
			 * path both armed to advance it. */
			c->video_packets = 0;
			c->video_bytes = 0;
			c->keyframes_seen = 0;
			c->frames_output = 0;
			c->decode_errors = 0;
			c->first_frame_ns = 0;
			c->no_output_warned = false;
			c->sps_logged = false;
			c->hw_retry = 0;
			c->packets_at_decoder = c->video_packets;
			c->next_decoder_attempt = 0;
			/* A dump file holds ONE codec (its extension says
			 * which); the new codec opens a fresh file. */
			dump_close(c);
			/* Diagnostics measure per decode-stream from here. */
			c->connected_ns = os_gettime_ns();
		}
		break;
	}
	case OBSC_PKT_VIDEO: {
		c->video_packets++;
		c->video_bytes += hdr->payload_size;

		/* Reference-absent check: the mic role is fixed for the
		 * stream's lifetime (the app starts capture with the
		 * stream), so a few seconds of live video with no type-9
		 * packet is conclusive, not just early. */
		if (!c->ref_absent_handled && !c->is_screen &&
		    c->ref_audio_packets == 0 && c->first_frame_ns &&
		    os_gettime_ns() - c->first_frame_ns > 5000000000ULL) {
			c->ref_absent_handled = true;
			lipsync_reference_absent(s);
		}
		bool keyframe = (hdr->flags & OBSC_FLAG_KEYFRAME) != 0;
		if (keyframe)
			c->keyframes_seen++;

		if (keyframe && !c->sps_logged &&
		    c->codec_id == AV_CODEC_ID_H264) {
			c->sps_logged = true;
			log_h264_sps(payload, hdr->payload_size);
		}

		if (s->dump_stream) {
			if (!c->dump_file && !c->dump_tried && keyframe)
				dump_open(c);
			if (c->dump_file &&
			    fwrite(payload, 1, hdr->payload_size,
				   c->dump_file) != hdr->payload_size) {
				blog(LOG_WARNING,
				     "[lenslink] stream dump: write failed "
				     "(disk full?), stopping the dump");
				dump_close(c);
				c->dump_tried = true;
			}
		} else if (c->dump_file) {
			/* Toggled off mid-stream: finish the file cleanly. */
			dump_close(c);
		}

		if (!c->decoder) {
			/* Join on a keyframe so the decoder starts clean. */
			if (!keyframe)
				break;
			/* Creation can fail persistently (e.g. no HEVC
			 * decoder in this FFmpeg build). Dropping the
			 * connection would just reconnect-churn as fast as
			 * the LAN allows; keep the link and retry with a
			 * cooldown instead. */
			uint64_t now = os_gettime_ns();
			if (now < c->next_decoder_attempt)
				break;
			c->decoder = h264_decoder_create(
				c->codec_id != AV_CODEC_ID_NONE
					? c->codec_id
					: AV_CODEC_ID_H264,
				s->hw_decode, c->hw_retry);
			/* GPU pipeline: frames come out as AVFrames for the
			 * render thread instead of obs_source_output_video.
			 * Set before the first decode (macOS picks its BGRA
			 * surface pool in the first get_format). */
			if (c->decoder && g_gpu_pipeline_mode)
				h264_decoder_set_frame_sink(
					c->decoder, gpu_pipeline_sink, s);
			if (!c->decoder) {
				c->next_decoder_attempt =
					now + 5000000000ULL;
				set_status(s, "%s",
					   T_("Status.DecoderFailed"));
				break;
			}
			c->next_decoder_attempt = 0;
			c->packets_at_decoder = c->video_packets;
		}
		if (!h264_decoder_decode(c->decoder, s->source, payload,
					 hdr->payload_size, hdr->pts_ns)) {
			c->decode_errors++;
			if (h264_decoder_is_hw(c->decoder)) {
				/* This GPU path misbehaved on this stream:
				 * advance to the next hardware API (then
				 * software once the list is exhausted). */
				c->hw_retry =
					h264_decoder_hw_index(c->decoder) + 1;
				blog(LOG_WARNING,
				     "[lenslink] %s decode error — trying "
				     "the next decoder",
				     h264_decoder_hw_name(c->decoder));
			} else {
				blog(LOG_WARNING,
				     "[lenslink] decoder error, resetting");
			}
			h264_decoder_destroy(c->decoder);
			c->decoder = NULL;
			break;
		}
		latency_on_frame(&c->lat, hdr->pts_ns, os_gettime_ns());

		/* First decoded frame is the key signal: if we see this, decode
		 * works and any "black" is downstream (OBS transform/render). If
		 * we never see it despite a keyframe going in, decode is the
		 * problem. */
		uint64_t out = h264_decoder_frames_output(c->decoder);
		if (out > c->frames_output) {
			c->frames_output = out;
			if (c->first_frame_ns == 0) {
				c->first_frame_ns = os_gettime_ns();
				int w = 0, h = 0;
				const char *fmt = "?";
				h264_decoder_last_frame(c->decoder, &w, &h,
							&fmt);
				blog(LOG_INFO,
				     "[lenslink] first decoded frame: %dx%d %s "
				     "(%s) after %llu ms, %llu pkt(s)",
				     w, h, fmt,
				     h264_decoder_is_hw(c->decoder) ? "GPU"
								    : "CPU",
				     (unsigned long long)((c->first_frame_ns -
							   c->connected_ns) /
							  1000000ULL),
				     (unsigned long long)c->video_packets);
			}
		}

		/* Watchdog: a keyframe went in and packets keep coming, but the
		 * decoder has produced nothing. The hardware path silently
		 * accepts packets yet emits no frames for some streams — notably
		 * tall, non-16-aligned screen-mirror resolutions on D3D11VA
		 * (e.g. 886x1918). avcodec_send_packet succeeds and
		 * avcodec_receive_frame just returns EAGAIN forever, so decode
		 * "succeeds" while OBS stays black. Advance to the next
		 * hardware API (software once the list is exhausted) instead
		 * of leaving a black source; the walk restarts from the best
		 * API on the next connection or codec switch. ~10 packets is
		 * ~1/6 s at 60 fps — long enough not to trip on a decoder's
		 * normal one-frame startup, short enough to recover quickly.
		 * The counters aren't reset on fallback, so if the next API
		 * also emits nothing this fires again and keeps walking. */
		if (c->frames_output == 0 && c->keyframes_seen > 0 &&
		    c->video_packets - c->packets_at_decoder >= 10) {
			if (c->decoder && h264_decoder_is_hw(c->decoder)) {
				blog(LOG_WARNING,
				     "[lenslink] %s decoder produced no "
				     "frames after %llu packets — trying "
				     "the next decoder",
				     h264_decoder_hw_name(c->decoder),
				     (unsigned long long)c->video_packets);
				c->hw_retry =
					h264_decoder_hw_index(c->decoder) + 1;
				h264_decoder_destroy(c->decoder);
				c->decoder = NULL;
				/* Recreated in software on the next keyframe;
				 * ask for one now so recovery is immediate
				 * instead of waiting up to ~2 s for the
				 * encoder's periodic keyframe. */
				request_keyframe(c);
			} else if (!c->no_output_warned) {
				c->no_output_warned = true;
				blog(LOG_WARNING,
				     "[lenslink] decoded 0 frames after %llu "
				     "packets incl. a keyframe — waiting for a "
				     "keyframe to (re)start decoding",
				     (unsigned long long)c->video_packets);
			}
		}
		break;
	}
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
		c->ref_audio_packets++;
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
	case OBSC_PKT_SCREEN_AUDIO: {
		/* Playable audio: screen-mirror system audio, or — when the
		 * app's "Send phone mic to OBS" option is on — the phone mic
		 * on a camera connection. Same wire format either way. */
		/* 48 kHz stereo S16LE interleaved, played as the source's
		 * audio. pts is in the same clock as the video frames, so
		 * OBS keeps A/V aligned for this async source. */
		size_t bytes_per_frame =
			OBSC_SCREEN_AUDIO_CHANNELS * sizeof(int16_t);
		struct obs_source_audio audio = {0};
		audio.data[0] = payload;
		audio.frames = (uint32_t)(hdr->payload_size / bytes_per_frame);
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_16BIT;
		audio.samples_per_sec = OBSC_SCREEN_AUDIO_SAMPLE_RATE;
		audio.timestamp = hdr->pts_ns;
		if (audio.frames) {
			obs_source_output_audio(s->source, &audio);
			c->audio_packets++;
			c->audio_frames += audio.frames;
			/* Peak level for the heartbeat: distinguishes "real
			 * audio arriving" from "a healthy stream of silence"
			 * (DRM apps mute themselves during broadcasts, so the
			 * packets keep flowing with nothing in them). */
			if (s->diagnostics) {
				const int16_t *pcm = (const int16_t *)payload;
				size_t n = hdr->payload_size / 2;
				for (size_t i = 0; i < n; i++) {
					int v = abs((int)pcm[i]);
					if (v > c->audio_peak)
						c->audio_peak = v;
				}
			}
		}
		break;
	}
	case OBSC_PKT_DIAG:
		/* The phone/extension's own counters, surfaced in the OBS log so
		 * the whole pipeline is visible in one place. */
		if (s->diagnostics && hdr->payload_size) {
			int len = hdr->payload_size < 400
					  ? (int)hdr->payload_size
					  : 400;
			blog(LOG_INFO, "[lenslink][phone] %.*s", len,
			     (const char *)payload);
		}
		break;
	default:
		blog(LOG_WARNING, "[lenslink] unknown packet type %d",
		     hdr->type);
		break;
	}
	return true;
}

static bool client_read(struct ios_camera_source *s, struct client_state *c)
{
	/* Receive straight into the buffer tail (no bounce copy), then
	 * parse in place with an offset and compact once at the end. */
	recv_buf_reserve(&c->buf, RECV_CHUNK);
	int n = (int)recv(c->sock, (char *)c->buf.data + c->buf.len,
			  RECV_CHUNK, 0);
	if (n == 0) {
		blog(LOG_INFO, "[lenslink] connection closed by device");
		return false;
	}
	if (n < 0) {
		blog(LOG_INFO, "[lenslink] recv error %d", net_last_error());
		return false;
	}
	c->buf.len += (size_t)n;

	size_t off = 0;
	bool ok = true;
	while (c->buf.len - off >= OBSC_HEADER_SIZE) {
		struct obsc_header hdr;
		if (!obsc_parse_header(c->buf.data + off, &hdr)) {
			blog(LOG_WARNING,
			     "[lenslink] bad packet header, dropping client");
			ok = false;
			break;
		}

		size_t total = OBSC_HEADER_SIZE + hdr.payload_size;
		if (c->buf.len - off < total)
			break;

		if (!handle_packet(s, c, &hdr,
				   c->buf.data + off + OBSC_HEADER_SIZE)) {
			ok = false;
			break;
		}

		off += total;
	}

	recv_buf_consume(&c->buf, off);
	return ok;
}

/* TCP connect with a 3-second timeout; returns a non-blocking socket.
 * Checks `stop` so destroying the source (which joins this thread) isn't
 * stuck behind the full connect wait. */
static socket_t tcp_dial(const char *host, uint16_t port,
			 volatile bool *stop)
{
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	/* The app shows the phone's numeric IP, so this fast path is the
	 * normal one — and it avoids getaddrinfo, whose DNS timeout can
	 * block for tens of seconds with no way to interrupt it. */
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		char port_str[16];
		snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
		struct addrinfo hints = {0};
		struct addrinfo *res = NULL;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
			return OBSC_INVALID_SOCKET;
		memcpy(&addr, res->ai_addr, sizeof(addr));
		addr.sin_port = htons(port);
		freeaddrinfo(res);
	}

	socket_t s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == OBSC_INVALID_SOCKET)
		return OBSC_INVALID_SOCKET;

	net_set_nonblocking(s);
	int ret = connect(s, (struct sockaddr *)&addr, sizeof(addr));

	if (ret != 0) {
#ifdef _WIN32
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			goto fail;
#else
		if (errno != EINPROGRESS)
			goto fail;
#endif
		/* Wait in 100 ms slices so a stop request interrupts the
		 * connect instead of the destroyer waiting out 3 s. */
		bool writable = false;
		for (int i = 0; i < 30 && !(stop && *stop); i++) {
			int r = net_wait(s, NET_WAIT_WRITE, 100);
			if (r < 0)
				goto fail;
			if (r & NET_WAIT_WRITE) {
				writable = true;
				break;
			}
		}
		if (!writable)
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
 * Device-claim registry. All LensLink Camera sources live in one process, so a
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

/* Claim key for a USB device: its stable UDID when known, otherwise a
 * fallback on the (ephemeral) device id so a device that doesn't report a
 * UDID is still usable in auto mode. */
static void usb_device_key(char *out, size_t size,
			   const struct usbmux_device *d)
{
	if (d->udid[0])
		snprintf(out, size, "usb:%s", d->udid);
	else
		snprintf(out, size, "usbid:%ld", d->id);
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

		/* Paused while hidden: stay disconnected and unclaimed so the
		 * phone idles (and another source may take it). Cheap poll —
		 * show/hide land on OBS threads, so the loop just watches the
		 * flags rather than being signalled. */
		if (s->deactivate_hidden && !s->showing) {
			if (claimed) {
				device_release(s);
				claimed = false;
				claimed_key[0] = 0;
			}
			set_status(s, "%s", T_("Status.Paused"));
			sleep_ms_interruptible(s, 300);
			continue;
		}

		if (usb) {
			struct usbmux_device devs[16];
			int n = usbmux_list_devices(devs, 16);
			const char *pinned = s->usb_device;

			/* Find our claimed device in the current list (its
			 * ephemeral id may have changed on replug); drop the
			 * claim if the phone is gone. */
			int chosen = -1;
			if (claimed) {
				for (int i = 0; i < n; i++) {
					char k[80];
					usb_device_key(k, sizeof(k), &devs[i]);
					if (strcmp(k, claimed_key) == 0) {
						chosen = i;
						break;
					}
				}
				if (chosen < 0) {
					device_release(s);
					claimed = false;
					claimed_key[0] = 0;
				}
			}
			/* Claim the pinned phone if set, else the first
			 * attached phone no other source owns. */
			if (!claimed) {
				for (int i = 0; i < n; i++) {
					if (pinned[0] &&
					    strcmp(pinned, devs[i].udid) != 0)
						continue;
					char k[80];
					usb_device_key(k, sizeof(k), &devs[i]);
					if (device_claim(k, s)) {
						claimed = true;
						snprintf(claimed_key,
							 sizeof(claimed_key),
							 "%s", k);
						chosen = i;
						break;
					}
				}
			}
			if (!claimed || chosen < 0) {
				const char *why =
					pinned[0] ? T_("Status.WaitingPinned")
					: n > 0	  ? T_("Status.DeviceBusy")
						  : T_("Status.WaitingUSB");
				set_status(s, "%s", why);
				/* Phone not reachable: re-arm auto-start so
				 * it fires when the app comes (back) up. */
				if (++s->dial_failures >= 2)
					s->auto_start_armed = true;
				sleep_ms_interruptible(s, 1000);
				continue;
			}
			usb_id = devs[chosen].id;
			/* Distinct from WaitingUSB: the phone IS here, we're
			 * waiting for something on it to listen (camera app
			 * started, or a screen broadcast running). */
			set_status(s, "%s", T_("Status.USBFound"));
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
			sock = tcp_dial(s->host, OBSC_USB_PORT, &s->stop);
		}

		if (sock == OBSC_INVALID_SOCKET) {
			/* Unreachable — the app is closed, backgrounded, or
			 * not yet listening. Re-arm auto-start so the camera
			 * starts the moment the app becomes reachable again
			 * (two consecutive failures, not one, so a blip
			 * right after a manual stop can't retrigger it). */
			if (++s->dial_failures >= 2)
				s->auto_start_armed = true;
			sleep_ms_interruptible(s, 2000);
			continue;
		}

		s->dial_failures = 0;
		blog(LOG_INFO, "[lenslink] connected to device (%s)",
		     usb ? "USB" : "network");
		set_status(s, "%s", T_("Status.Connected"));

		struct client_state client = {.sock = sock};

		while (!s->stop) {
			if (s->deactivate_hidden && !s->showing) {
				/* Hidden: drop the live connection. With
				 * remote start in play, also stop the camera
				 * itself (best effort) and re-arm so showing
				 * the source starts it again — the phone
				 * truly idles instead of encoding into a
				 * dead link. */
				if (s->auto_start && !s->is_screen_source &&
				    !client.standby) {
					send_control_cmd(
						&client,
						"{\"cmd\":\"stop_stream\"}");
					client_flush(&client);
					s->auto_start_armed = true;
				}
				break;
			}
			int events = NET_WAIT_READ;
			if (client.out.len > 0)
				events |= NET_WAIT_WRITE;
			int ret = net_wait(sock, events, 200);
			if (ret < 0)
				break;
			if (client.out.len > 0)
				client_flush(&client);
			latency_tick(s, &client);
			control_tick(s, &client);
			diag_tick(s, &client);
			stats_tick(s, &client);
			if (client.out.failed)
				break;
			if ((ret & NET_WAIT_READ) &&
			    !client_read(s, &client))
				break;
		}

		blog(LOG_INFO, "[lenslink] device connection ended");
		bool wrong_kind = client.wrong_kind;
		client_disconnect(s, &client);

		/* Rejected for the wrong stream kind: the phone will keep
		 * offering the same stream, so an immediate redial would spin
		 * a reject loop at LAN speed. Back off; the status already
		 * says what to change. */
		if (wrong_kind)
			sleep_ms_interruptible(s, 3000);
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
		blog(LOG_ERROR, "[lenslink] failed to start server thread");
}

/* Visibility tracking for "deactivate when hidden": OBS calls these when
 * the source starts/stops being shown on any display. The dial-loop thread
 * polls the flag, so these just record state — no joins, no blocking. */
static void ios_camera_show(void *data)
{
	struct ios_camera_source *s = data;
	s->showing = true;
}

static void ios_camera_hide(void *data)
{
	struct ios_camera_source *s = data;
	s->showing = false;
}

static const char *ios_camera_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("IOSCameraSource");
}

static const char *lenslink_screen_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("ScreenSource");
}

/*
 * The Phone field is an editable combo, and OBS editable combos save the
 * DISPLAYED text, not the list item's value — picking a discovered phone
 * stores its whole "Name (192.168.1.23)" label as the host. Extract the
 * address from a trailing "(...)" when it looks like one; hand-typed IPs
 * and hostnames pass through untouched.
 */
static void extract_dial_host(const char *setting, char *out, size_t out_size)
{
	snprintf(out, out_size, "%s", setting);

	size_t len = strlen(out);
	if (len < 3 || out[len - 1] != ')')
		return;
	char *open = strrchr(out, '(');
	if (!open || open == out)
		return;

	/* Address-ish check: IPv4/IPv6/hostname chars only. A phone name
	 * that itself ends in parentheses shouldn't get mangled. */
	bool addr = true;
	for (const char *p = open + 1; p < out + len - 1; p++) {
		char c = *p;
		if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') &&
		    !(c >= 'A' && c <= 'Z') && c != '.' && c != ':' &&
		    c != '-' && c != '%') {
			addr = false;
			break;
		}
	}
	if (!addr || open + 1 == out + len - 1)
		return;

	size_t n = (size_t)(out + len - 1 - (open + 1));
	memmove(out, open + 1, n);
	out[n] = 0;
}

static void ios_camera_update(void *data, obs_data_t *settings)
{
	struct ios_camera_source *s = data;
	enum connection_mode mode =
		parse_mode(obs_data_get_string(settings, S_MODE));
	char host[128];
	extract_dial_host(obs_data_get_string(settings, S_HOST), host,
			  sizeof(host));

	/* Unbuffered async video: render the newest frame immediately
	 * instead of letting OBS smooth timestamps with a frame queue. */
	obs_source_set_async_unbuffered(
		s->source, obs_data_get_bool(settings, S_LOW_LATENCY));
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	s->diagnostics = lenslink_settings_diagnostics();
	s->dump_stream = lenslink_settings_dump_stream();
	s->deactivate_hidden =
		obs_data_get_bool(settings, S_DEACTIVATE_HIDDEN);
	s->auto_start = !s->is_screen_source &&
			obs_data_get_bool(settings, S_AUTO_START);

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

	/* Screen sources never run the web panel: nothing to control. The
	 * enable/port live in the plugin-wide settings now (Tools menu). */
	apply_web_settings(s);

	/* Turned off (or retargeted): undo the offset we applied. */
	if (was_syncing && !still_syncing && applied != 0) {
		obs_source_t *audio = obs_get_source_by_name(old_source);
		if (audio) {
			obs_source_set_sync_offset(audio, 0);
			obs_source_release(audio);
		}
	}

	const char *usb_device = obs_data_get_string(settings, S_USB_DEVICE);
	if (mode != s->conn_mode || strcmp(host, s->host) != 0 ||
	    strcmp(usb_device, s->usb_device) != 0) {
		stop_thread(s);
		s->conn_mode = mode;
		snprintf(s->host, sizeof(s->host), "%s", host);
		snprintf(s->usb_device, sizeof(s->usb_device), "%s",
			 usb_device);
		start_thread(s);
	}
}

#define SCREEN_SOURCE_ID "lenslink_screen_source"

static void *ios_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct ios_camera_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->is_screen_source =
		strcmp(obs_source_get_id(source), SCREEN_SOURCE_ID) == 0;

	/* Init locks/state before anything that might use them (the web
	 * thread reads status; the audio callback reads lipsync). */
	pthread_mutex_init(&s->status_mutex, NULL);
	pthread_mutex_init(&s->lipsync_mutex, NULL);
	pthread_mutex_init(&s->frame_mutex, NULL);
	dstr_init(&s->status);
	s->lipsync = lipsync_create();

	s->conn_mode = parse_mode(obs_data_get_string(settings, S_MODE));
	extract_dial_host(obs_data_get_string(settings, S_HOST), s->host,
			  sizeof(s->host));
	snprintf(s->usb_device, sizeof(s->usb_device), "%s",
		 obs_data_get_string(settings, S_USB_DEVICE));
	obs_source_set_async_unbuffered(
		source, obs_data_get_bool(settings, S_LOW_LATENCY));
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	s->diagnostics = lenslink_settings_diagnostics();
	s->dump_stream = lenslink_settings_dump_stream();
	s->deactivate_hidden =
		obs_data_get_bool(settings, S_DEACTIVATE_HIDDEN);
	s->auto_start = !s->is_screen_source &&
			obs_data_get_bool(settings, S_AUTO_START);
	/* Armed at creation: an app already open and idle when this source
	 * appears should start streaming right away. */
	s->auto_start_armed = true;
	s->audio_sync = obs_data_get_bool(settings, S_AUDIO_SYNC);
	s->audio_device_latency_ms =
		(int)obs_data_get_int(settings, S_AUDIO_LATENCY);
	s->auto_calibrate = obs_data_get_bool(settings, S_AUTO_CALIBRATE);
	s->auto_video_delay =
		obs_data_get_bool(settings, S_AUTO_VIDEO_DELAY);
	snprintf(s->audio_source, sizeof(s->audio_source), "%s",
		 obs_data_get_string(settings, S_AUDIO_SOURCE));

	/* Camera-only machinery: a screen mirror has no camera controls to
	 * serve and no camera latency to lip-sync against. */
	if (!s->is_screen_source) {
		if (s->audio_sync && s->auto_calibrate && s->audio_source[0])
			hook_audio(s, s->audio_source);
		apply_web_settings(s);
	}

	start_thread(s);
	health_register(s);
	return s;
}

static void ios_camera_destroy(void *data)
{
	struct ios_camera_source *s = data;

	/* First: the frontend UI must not snapshot a freeing source. */
	health_unregister(s);

	web_control_stop(s->web);
	unhook_audio(s);
	stop_thread(s);

	/* Undo the sync offset we applied to the user's audio source; a
	 * stale offset would silently persist after this source is gone.
	 * (The auto video-delay filter lives on this source and dies with
	 * it.) */
	if (s->applied_audio_offset != 0 && s->audio_source[0]) {
		obs_source_t *audio =
			obs_get_source_by_name(s->audio_source);
		if (audio) {
			obs_source_set_sync_offset(audio, 0);
			obs_source_release(audio);
		}
	}

	if (g_gpu_pipeline_mode) {
		obs_enter_graphics();
		gpu_pipeline_free(s);
		obs_leave_graphics();
	}

	for (int i = 0; i < s->control_count; i++)
		bfree(s->control_queue[i]);
	lipsync_destroy(s->lipsync);
	dstr_free(&s->status);
	pthread_mutex_destroy(&s->frame_mutex);
	pthread_mutex_destroy(&s->lipsync_mutex);
	pthread_mutex_destroy(&s->status_mutex);
	bfree(s);
}

static void ios_camera_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_MODE, MODE_DIAL);
	obs_data_set_default_string(settings, S_HOST, "");
	obs_data_set_default_string(settings, S_USB_DEVICE, "");
	obs_data_set_default_bool(settings, S_LOW_LATENCY, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_bool(settings, S_AUDIO_SYNC, false);
	obs_data_set_default_string(settings, S_AUDIO_SOURCE, "");
	obs_data_set_default_int(settings, S_AUDIO_LATENCY, 0);
	obs_data_set_default_bool(settings, S_AUTO_CALIBRATE, false);
	obs_data_set_default_bool(settings, S_AUTO_VIDEO_DELAY, false);
	obs_data_set_default_bool(settings, S_DEACTIVATE_HIDDEN, false);
	obs_data_set_default_bool(settings, S_AUTO_START, true);
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
	obs_property_set_visible(obs_properties_get(props, S_USB_DEVICE),
				 mode == CONN_DIAL_USB);
	return true;
}

/* Populates the USB-device picker with "Auto" plus every attached phone
 * (labelled by the last chars of its UDID), keeping the current pin even
 * if that device is currently unplugged. */
static void fill_usb_devices(obs_property_t *list, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T_("UsbDevice.Auto"), "");

	struct usbmux_device devs[16];
	int n = usbmux_list_devices(devs, 16);
	bool current_seen = !current || !current[0];
	for (int i = 0; i < n; i++) {
		if (!devs[i].udid[0])
			continue;
		size_t len = strlen(devs[i].udid);
		const char *tail =
			devs[i].udid + (len > 6 ? len - 6 : 0);
		char label[96];
		snprintf(label, sizeof(label), "iPhone/iPad (…%s)", tail);
		obs_property_list_add_string(list, label, devs[i].udid);
		if (current && strcmp(current, devs[i].udid) == 0)
			current_seen = true;
	}
	/* Keep a pinned-but-unplugged device selectable. */
	if (!current_seen) {
		char label[96];
		snprintf(label, sizeof(label), "%s (not connected)", current);
		obs_property_list_add_string(list, label, current);
	}
}

/* Remote start/stop: queue the command for the connected app. Queued like
 * any web-panel control, so it rides the existing control channel and is
 * a no-op when nothing is connected. */
static bool start_camera_clicked(obs_properties_t *props,
				 obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ios_camera_source *s = data;
	static const char json[] = "{\"cmd\":\"start_stream\"}";

	if (s)
		ios_camera_enqueue_control(s, json, sizeof(json) - 1);
	return false;
}

static bool stop_camera_clicked(obs_properties_t *props,
				obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct ios_camera_source *s = data;
	static const char json[] = "{\"cmd\":\"stop_stream\"}";

	if (s)
		ios_camera_enqueue_control(s, json, sizeof(json) - 1);
	return false;
}

/* Property sheets for the two source types share the connection and decode
 * settings; only the camera type gets the lip-sync group and the web panel
 * (neither applies to a screen mirror). */
static obs_properties_t *build_properties(struct ios_camera_source *s,
					   bool screen)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode = obs_properties_add_list(
		props, S_MODE, T_("Mode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, T_("Mode.Dial"), MODE_DIAL);
	obs_property_list_add_string(mode, T_("Mode.USB"), MODE_USB);
	obs_property_set_modified_callback(mode, mode_modified);

	/* Editable combo: type an IP as before, or pick a phone found via
	 * Bonjour. The short blocking browse only runs when the properties
	 * sheet opens — same pattern as the USB device scan below. */
	obs_property_t *host = obs_properties_add_list(
		props, S_HOST, T_("Host"), OBS_COMBO_TYPE_EDITABLE,
		OBS_COMBO_FORMAT_STRING);
	struct mdns_result phones[8];
	int found = mdns_browse(LENSLINK_MDNS_SERVICE, 500, phones, 8);
	for (int i = 0; i < found; i++) {
		char label[144];
		snprintf(label, sizeof(label), "%.63s (%.63s)", phones[i].name,
			 phones[i].host);
		/* name == value on purpose: OBS editable combos save the
		 * displayed text, so the stored setting IS the label —
		 * extract_dial_host() digs the address back out when
		 * dialing. Distinct values would just desync the two. */
		obs_property_list_add_string(host, label, label);
	}

	obs_property_t *usb_dev = obs_properties_add_list(
		props, S_USB_DEVICE, T_("UsbDevice"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	fill_usb_devices(usb_dev, s ? s->usb_device : "");

	obs_properties_add_bool(props, S_LOW_LATENCY, T_("LowLatency"));
	obs_properties_add_bool(props, S_HW_DECODE, T_("HwDecode"));

	obs_property_t *deact = obs_properties_add_bool(
		props, S_DEACTIVATE_HIDDEN, T_("DeactivateHidden"));
	obs_property_set_long_description(deact, T_("DeactivateHidden.Desc"));

	if (!screen) {
		/* Remote start: only cameras — iOS forbids starting a screen
		 * broadcast without a tap on the phone. */
		obs_property_t *auto_start = obs_properties_add_bool(
			props, S_AUTO_START, T_("AutoStart"));
		obs_property_set_long_description(auto_start,
						  T_("AutoStart.Desc"));
		obs_properties_add_button(props, "start_camera",
					  T_("StartCamera"),
					  start_camera_clicked);
		obs_properties_add_button(props, "stop_camera",
					  T_("StopCamera"),
					  stop_camera_clicked);

		obs_property_t *audio_list = obs_properties_add_list(
			props, S_AUDIO_SOURCE, T_("AudioSource"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(audio_list,
					     T_("AudioSource.None"), "");
		obs_enum_sources(add_audio_source, audio_list);
		obs_properties_add_bool(props, S_AUDIO_SYNC, T_("AudioSync"));
		obs_properties_add_bool(props, S_AUTO_CALIBRATE,
					T_("AutoCalibrate"));
		obs_properties_add_bool(props, S_AUTO_VIDEO_DELAY,
					T_("AutoVideoDelay"));
		obs_property_t *audio_lat = obs_properties_add_int(
			props, S_AUDIO_LATENCY, T_("AudioLatency"), 0, 500, 1);
		obs_property_int_set_suffix(audio_lat, " ms");

	}

	/* Web panel / diagnostics / stream dump moved to the plugin-wide
	 * settings (Tools -> LensLink Settings): they never made sense per
	 * source — one web port, one log. */

	obs_property_t *status = obs_properties_add_text(
		props, "status_info", T_("Status"), OBS_TEXT_INFO);
	if (s) {
		pthread_mutex_lock(&s->status_mutex);
		obs_property_set_long_description(
			status, s->status.array ? s->status.array : "");
		pthread_mutex_unlock(&s->status_mutex);
	}

	obs_properties_add_text(props, "help_info",
				screen ? T_("HelpTextScreen") : T_("HelpText"),
				OBS_TEXT_INFO);

	return props;
}

static obs_properties_t *ios_camera_get_properties(void *data)
{
	return build_properties(data, false);
}

static obs_properties_t *lenslink_screen_get_properties(void *data)
{
	return build_properties(data, true);
}

/* Two registered source types share this whole engine; instances only
 * differ by `is_screen_source` (set from the id at create). The camera id
 * predates the split and must never change — it's baked into users' scene
 * collections. A camera source still *plays* a screen stream if the phone
 * sends one (and vice versa), so pre-split setups keep working; the status
 * line just nudges toward the matching type. */

/* ------------------------------------------------------------------ */
/* GPU (beta) pipeline: sync-source rendering                          */
/* ------------------------------------------------------------------ */

/* Decode thread -> render thread hand-off: newest frame wins. */
static void gpu_pipeline_sink(void *ud, AVFrame *frame)
{
	struct ios_camera_source *s = ud;
	pthread_mutex_lock(&s->frame_mutex);
	if (s->pending_frame)
		av_frame_free(&s->pending_frame);
	s->pending_frame = frame;
	s->frame_width = (uint32_t)frame->width;
	s->frame_height = (uint32_t)frame->height;
	pthread_mutex_unlock(&s->frame_mutex);
}

/* Blank the source (disconnect/stream end): both frames go; dims reset
 * so get_width/get_height report nothing to draw. */
static void gpu_pipeline_clear(struct ios_camera_source *s)
{
	if (!g_gpu_pipeline_mode)
		return;
	pthread_mutex_lock(&s->frame_mutex);
	if (s->pending_frame)
		av_frame_free(&s->pending_frame);
	if (s->current_frame)
		av_frame_free(&s->current_frame);
	s->frame_width = 0;
	s->frame_height = 0;
	pthread_mutex_unlock(&s->frame_mutex);
}

static uint32_t ios_camera_get_width(void *data)
{
	struct ios_camera_source *s = data;
	return s->frame_width;
}

static uint32_t ios_camera_get_height(void *data)
{
	struct ios_camera_source *s = data;
	return s->frame_height;
}

/* The NV12/I420 draw effect, shared by every source (graphics thread). */
static gs_effect_t *g_yuv_effect = NULL;
static bool g_yuv_effect_tried = false;

static gs_effect_t *yuv_effect(void)
{
	if (!g_yuv_effect && !g_yuv_effect_tried) {
		g_yuv_effect_tried = true;
		char *path = obs_module_file("nv12.effect");
		if (path) {
			g_yuv_effect = gs_effect_create_from_file(path, NULL);
			bfree(path);
		}
		if (!g_yuv_effect)
			blog(LOG_ERROR,
			     "[lenslink] gpu pipeline: nv12.effect missing — "
			     "YUV frames cannot be drawn");
	}
	return g_yuv_effect;
}

/* CPU-upload fallback: software-decoded frames (or a failed interop) are
 * uploaded into dynamic textures — the same work OBS's async path would
 * do, so "fallback" here means "no worse than the standard pipeline". */
static bool upload_sw_frame(struct ios_camera_source *s, const AVFrame *f,
			    struct gpu_frame_map_result *out)
{
	int fmt = f->format;
	if (fmt != AV_PIX_FMT_NV12 && fmt != AV_PIX_FMT_YUV420P &&
	    fmt != AV_PIX_FMT_YUVJ420P)
		return false;

	uint32_t w = (uint32_t)f->width, h = (uint32_t)f->height;
	if (!s->cpu_tex[0] || s->cpu_tex_w != w || s->cpu_tex_h != h ||
	    s->cpu_tex_fmt != fmt) {
		for (int i = 0; i < 3; i++) {
			if (s->cpu_tex[i]) {
				gs_texture_destroy(s->cpu_tex[i]);
				s->cpu_tex[i] = NULL;
			}
		}
		s->cpu_tex[0] =
			gs_texture_create(w, h, GS_R8, 1, NULL, GS_DYNAMIC);
		if (fmt == AV_PIX_FMT_NV12) {
			s->cpu_tex[1] = gs_texture_create(w / 2, h / 2, GS_R8G8,
							  1, NULL, GS_DYNAMIC);
		} else {
			s->cpu_tex[1] = gs_texture_create(w / 2, h / 2, GS_R8,
							  1, NULL, GS_DYNAMIC);
			s->cpu_tex[2] = gs_texture_create(w / 2, h / 2, GS_R8,
							  1, NULL, GS_DYNAMIC);
		}
		if (!s->cpu_tex[0] || !s->cpu_tex[1] ||
		    (fmt != AV_PIX_FMT_NV12 && !s->cpu_tex[2]))
			return false;
		s->cpu_tex_w = w;
		s->cpu_tex_h = h;
		s->cpu_tex_fmt = fmt;
	}

	gs_texture_set_image(s->cpu_tex[0], f->data[0],
			     (uint32_t)f->linesize[0], false);
	gs_texture_set_image(s->cpu_tex[1], f->data[1],
			     (uint32_t)f->linesize[1], false);
	if (fmt != AV_PIX_FMT_NV12)
		gs_texture_set_image(s->cpu_tex[2], f->data[2],
				     (uint32_t)f->linesize[2], false);

	out->tex[0] = s->cpu_tex[0];
	out->tex[1] = s->cpu_tex[1];
	out->rgba = false;
	out->width = w;
	out->height = h;
	return true;
}

static void ios_camera_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct ios_camera_source *s = data;

	/* Adopt the newest decoded frame, if any. */
	pthread_mutex_lock(&s->frame_mutex);
	AVFrame *fresh = s->pending_frame;
	s->pending_frame = NULL;
	pthread_mutex_unlock(&s->frame_mutex);

	if (fresh) {
		if (s->current_frame)
			av_frame_free(&s->current_frame);
		s->current_frame = fresh;

		uint64_t bench_start = os_gettime_ns();
		if (!s->gpu)
			s->gpu = gpu_frame_ctx_create();
		s->gpu_mapped = false;
		if (gpu_frame_supported(fresh))
			s->gpu_mapped =
				gpu_frame_map(s->gpu, fresh, &s->gpu_map);
		if (!s->gpu_mapped && !upload_sw_frame(s, fresh, &s->gpu_map)) {
			/* Neither mappable nor uploadable: drop it, or later
			 * renders would redraw a stale mapping against this
			 * frame's dimensions. */
			av_frame_free(&s->current_frame);
			return;
		}
		/* Benchmark: this pipeline's per-frame cost is the texture
		 * map (or, on fallback, the CPU upload). Bytes cross system
		 * memory only on the fallback path. */
		lenslink_bench_frame(os_gettime_ns() - bench_start,
				     s->gpu_mapped ? 0
						   : (size_t)s->gpu_map.width *
							     s->gpu_map.height *
							     3 / 2,
				     (int)s->gpu_map.width,
				     (int)s->gpu_map.height);
	}
	if (!s->current_frame)
		return;

	bool locked = s->gpu_mapped ? gpu_frame_lock(s->gpu) : true;
	if (!locked)
		return;

	if (s->gpu_map.rgba) {
		gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");
		gs_effect_set_texture(image, s->gpu_map.tex[0]);
		while (gs_effect_loop(eff, "Draw"))
			gs_draw_sprite(s->gpu_map.tex[0], 0, s->gpu_map.width,
				       s->gpu_map.height);
	} else {
		gs_effect_t *eff = yuv_effect();
		if (eff) {
			bool i420 = !s->gpu_mapped &&
				    s->cpu_tex_fmt != AV_PIX_FMT_NV12;
			gs_effect_set_texture(
				gs_effect_get_param_by_name(eff, "y_tex"),
				s->gpu_map.tex[0]);
			gs_effect_set_texture(
				gs_effect_get_param_by_name(eff, "uv_tex"),
				s->gpu_map.tex[1]);
			if (i420)
				gs_effect_set_texture(
					gs_effect_get_param_by_name(eff,
								    "v_tex"),
					s->cpu_tex[2]);
			while (gs_effect_loop(eff, i420 ? "DrawI420" : "Draw"))
				gs_draw_sprite(s->gpu_map.tex[0], 0,
					       s->gpu_map.width,
					       s->gpu_map.height);
		}
	}

	if (s->gpu_mapped)
		gpu_frame_unlock(s->gpu);
}

/* Frees everything the render path owns; called from destroy with the
 * graphics context entered. */
static void gpu_pipeline_free(struct ios_camera_source *s)
{
	gpu_frame_ctx_destroy(s->gpu);
	s->gpu = NULL;
	for (int i = 0; i < 3; i++) {
		if (s->cpu_tex[i]) {
			gs_texture_destroy(s->cpu_tex[i]);
			s->cpu_tex[i] = NULL;
		}
	}
	if (s->pending_frame)
		av_frame_free(&s->pending_frame);
	if (s->current_frame)
		av_frame_free(&s->current_frame);
}

/* Called from plugin-main.c at module load, BEFORE registration, when
 * the plugin-wide "GPU pipeline (beta)" setting is on: the sources
 * become self-rendering sync sources instead of async pixel-pushers. */
void lenslink_sources_use_gpu_pipeline(struct obs_source_info *info)
{
	g_gpu_pipeline_mode = true;
	info->output_flags &= ~(uint32_t)OBS_SOURCE_ASYNC_VIDEO;
	info->output_flags |= OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info->video_render = ios_camera_video_render;
	info->get_width = ios_camera_get_width;
	info->get_height = ios_camera_get_height;
}

struct obs_source_info ios_camera_source_info = {
	.id = "ios_camera_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* AUDIO flag: the camera plays audio when the app's "Send phone mic
	 * to OBS" option is on (packet type 10, same as screen audio). The
	 * mixer meter is idle for users who leave that off — the standard
	 * trade for any camera source that *can* carry audio. */
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ios_camera_get_name,
	.create = ios_camera_create,
	.destroy = ios_camera_destroy,
	.update = ios_camera_update,
	.get_defaults = ios_camera_get_defaults,
	.get_properties = ios_camera_get_properties,
	.show = ios_camera_show,
	.hide = ios_camera_hide,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

struct obs_source_info lenslink_screen_source_info = {
	.id = SCREEN_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = lenslink_screen_get_name,
	.create = ios_camera_create,
	.destroy = ios_camera_destroy,
	.update = ios_camera_update,
	.get_defaults = ios_camera_get_defaults,
	.get_properties = lenslink_screen_get_properties,
	.show = ios_camera_show,
	.hide = ios_camera_hide,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
