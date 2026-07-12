#include "lipsync.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* 2 ms envelope frames (500 Hz). Fine enough for lip sync (perception
 * threshold is tens of ms) and cheap to correlate. */
#define FRAME_NS 2000000LL
#define RING 3072              /* ~6.1 s of envelope history */
#define MAX_LAG_FRAMES 300     /* search +/- 600 ms */
#define MIN_OVERLAP_FRAMES 400 /* need ~0.8 s of shared audio */
#define MIN_ENERGY 1e-4        /* reject near-silent windows */

/* A 500 Hz amplitude-envelope stream, accumulated frame by frame. */
struct env_stream {
	/* current (not-yet-flushed) frame accumulator */
	int64_t cur_frame;
	double sum_sq;
	uint32_t n;
	bool have_cur;

	/* ring of flushed frames */
	int64_t frame[RING];
	float energy[RING];
	int head; /* next write index */
	int count;
};

struct lipsync {
	struct env_stream ref;
	struct env_stream mic;
};

struct lipsync *lipsync_create(void)
{
	return calloc(1, sizeof(struct lipsync));
}

void lipsync_destroy(struct lipsync *ls)
{
	free(ls);
}

void lipsync_reset(struct lipsync *ls)
{
	memset(ls, 0, sizeof(*ls));
}

static void stream_push(struct env_stream *s, int64_t frame, float energy)
{
	s->frame[s->head] = frame;
	s->energy[s->head] = energy;
	s->head = (s->head + 1) % RING;
	if (s->count < RING)
		s->count++;
}

static void stream_add_sample(struct env_stream *s, double sample,
			      int64_t t_ns)
{
	int64_t frame = t_ns / FRAME_NS;

	if (!s->have_cur) {
		s->have_cur = true;
		s->cur_frame = frame;
		s->sum_sq = 0;
		s->n = 0;
	}

	if (frame != s->cur_frame) {
		float rms = s->n ? (float)sqrt(s->sum_sq / s->n) : 0.0f;
		stream_push(s, s->cur_frame, rms);
		s->cur_frame = frame;
		s->sum_sq = 0;
		s->n = 0;
	}

	s->sum_sq += sample * sample;
	s->n++;
}

void lipsync_add_reference(struct lipsync *ls, const int16_t *pcm,
			   size_t samples, uint32_t sample_rate,
			   uint64_t start_time_ns)
{
	if (!sample_rate)
		return;
	double ns_per = 1.0e9 / (double)sample_rate;
	for (size_t i = 0; i < samples; i++) {
		int64_t t = (int64_t)(start_time_ns + (uint64_t)(i * ns_per));
		stream_add_sample(&ls->ref, pcm[i] / 32768.0, t);
	}
}

void lipsync_add_mic(struct lipsync *ls, const float *mono, size_t count,
		     uint32_t sample_rate, uint64_t start_time_ns)
{
	if (!sample_rate)
		return;
	double ns_per = 1.0e9 / (double)sample_rate;
	for (size_t i = 0; i < count; i++) {
		int64_t t = (int64_t)(start_time_ns + (uint64_t)(i * ns_per));
		stream_add_sample(&ls->mic, mono[i], t);
	}
}

static bool stream_bounds(const struct env_stream *s, int64_t *lo, int64_t *hi)
{
	if (s->count == 0)
		return false;
	int64_t mn = INT64_MAX, mx = INT64_MIN;
	for (int i = 0; i < s->count; i++) {
		int idx = (s->head - 1 - i + 2 * RING) % RING;
		int64_t f = s->frame[idx];
		if (f < mn)
			mn = f;
		if (f > mx)
			mx = f;
	}
	*lo = mn;
	*hi = mx + 1;
	return true;
}

/* Zero-filled dense energy over [lo, hi) frames. */
static void build_dense(const struct env_stream *s, int64_t lo, int64_t hi,
			float *out)
{
	memset(out, 0, sizeof(float) * (size_t)(hi - lo));
	for (int i = 0; i < s->count; i++) {
		int idx = (s->head - 1 - i + 2 * RING) % RING;
		int64_t f = s->frame[idx];
		if (f >= lo && f < hi)
			out[f - lo] = s->energy[idx];
	}
}

static void subtract_mean(float *a, int n)
{
	double sum = 0;
	for (int i = 0; i < n; i++)
		sum += a[i];
	float mean = (float)(sum / n);
	for (int i = 0; i < n; i++)
		a[i] -= mean;
}

bool lipsync_estimate(struct lipsync *ls, int64_t *mic_delay_ns,
		      double *confidence)
{
	int64_t rlo, rhi, mlo, mhi;
	if (!stream_bounds(&ls->ref, &rlo, &rhi) ||
	    !stream_bounds(&ls->mic, &mlo, &mhi))
		return false;

	/* Analysis window = the frames both streams cover. */
	int64_t lo = rlo > mlo ? rlo : mlo;
	int64_t hi = rhi < mhi ? rhi : mhi;
	int64_t win = hi - lo;
	if (win < MIN_OVERLAP_FRAMES + MAX_LAG_FRAMES)
		return false;

	/* Reference over the window; mic padded by the lag range so shifted
	 * lookups stay in bounds. */
	int L = (int)win;
	int ML = L + 2 * MAX_LAG_FRAMES;
	float *p = malloc(sizeof(float) * (size_t)L);
	float *m = malloc(sizeof(float) * (size_t)ML);
	if (!p || !m) {
		free(p);
		free(m);
		return false;
	}

	build_dense(&ls->ref, lo, hi, p);
	build_dense(&ls->mic, lo - MAX_LAG_FRAMES, hi + MAX_LAG_FRAMES, m);

	subtract_mean(p, L);
	subtract_mean(m, ML);

	double norm_p = 0;
	for (int i = 0; i < L; i++)
		norm_p += (double)p[i] * p[i];

	if (norm_p < MIN_ENERGY) {
		free(p);
		free(m);
		return false; /* reference essentially silent */
	}

	double best = -1e30;
	int best_lag = 0;
	/* score(lag) = sum p[i] * m[i+lag]; peaks at lag == mic delay. */
	for (int lag = -MAX_LAG_FRAMES; lag <= MAX_LAG_FRAMES; lag++) {
		double dot = 0, norm_m = 0;
		int base = lag + MAX_LAG_FRAMES; /* index into padded m */
		for (int i = 0; i < L; i++) {
			float mv = m[i + base];
			dot += (double)p[i] * mv;
			norm_m += (double)mv * mv;
		}
		if (norm_m < MIN_ENERGY)
			continue;
		double ncc = dot / sqrt(norm_p * norm_m);
		if (ncc > best) {
			best = ncc;
			best_lag = lag;
		}
	}

	free(p);
	free(m);

	/* A peak pinned to the search edge means the true lag is outside the
	 * window — don't trust it. */
	if (best_lag <= -MAX_LAG_FRAMES || best_lag >= MAX_LAG_FRAMES)
		return false;

	*mic_delay_ns = (int64_t)best_lag * FRAME_NS;
	*confidence = best;
	return true;
}
