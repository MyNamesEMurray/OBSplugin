/*
 * Lip-sync auto-calibration by audio cross-correlation.
 *
 * The phone sends a low-rate audio *reference* that is synced to its video
 * by construction (shared capture clock). We also tap the streamer's real
 * microphone in OBS. Both signals contain the same acoustic events (the
 * streamer's voice); cross-correlating their amplitude envelopes recovers
 * the real mic's latency relative to the video, with no manual entry.
 *
 * The two feeds arrive on different threads (network vs OBS audio), so the
 * caller must serialize access with its own mutex.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct lipsync;

struct lipsync *lipsync_create(void);
void lipsync_destroy(struct lipsync *ls);
void lipsync_reset(struct lipsync *ls);

/* Phone reference PCM (mono), sample timestamps derived from start_time_ns
 * — already converted to the plugin's clock domain. */
void lipsync_add_reference(struct lipsync *ls, const int16_t *pcm,
			   size_t samples, uint32_t sample_rate,
			   uint64_t start_time_ns);

/* Real-mic samples (mono float), first sample at start_time_ns (OBS/plugin
 * clock). */
void lipsync_add_mic(struct lipsync *ls, const float *mono, size_t count,
		     uint32_t sample_rate, uint64_t start_time_ns);

/*
 * Estimates the mic's delay relative to the reference (== the mic's own
 * path latency L_mic). Returns false when correlation confidence is too
 * low (e.g. during silence). confidence is in [0, 1].
 */
bool lipsync_estimate(struct lipsync *ls, int64_t *mic_delay_ns,
		      double *confidence);
