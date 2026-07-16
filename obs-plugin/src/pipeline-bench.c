#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "pipeline-bench.h"
#include "plugin-settings.h"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_frames;
static uint64_t g_cost_total_ns;
static uint64_t g_cost_max_ns;
static uint64_t g_bytes;
static int g_width, g_height;
static uint64_t g_last_log_ns;

/* Process CPU usage, sampled the same way OBS's own Stats dock does. */
static os_cpu_usage_info_t *g_cpu;

void lenslink_bench_frame(uint64_t cost_ns, size_t bytes_copied, int width,
			  int height)
{
	if (!lenslink_settings_benchmark())
		return;

	pthread_mutex_lock(&g_mutex);
	g_frames++;
	g_cost_total_ns += cost_ns;
	if (cost_ns > g_cost_max_ns)
		g_cost_max_ns = cost_ns;
	g_bytes += bytes_copied;
	g_width = width;
	g_height = height;
	pthread_mutex_unlock(&g_mutex);
}

void lenslink_bench_maybe_log(void)
{
	if (!lenslink_settings_benchmark())
		return;

	pthread_mutex_lock(&g_mutex);
	uint64_t now = os_gettime_ns();
	if (g_last_log_ns == 0) {
		/* First tick with the benchmark on: start the window (and
		 * the CPU sampler) instead of logging garbage. */
		g_last_log_ns = now;
		g_frames = 0;
		g_cost_total_ns = 0;
		g_cost_max_ns = 0;
		g_bytes = 0;
		if (!g_cpu)
			g_cpu = os_cpu_usage_info_start();
		pthread_mutex_unlock(&g_mutex);
		return;
	}
	uint64_t window_ns = now - g_last_log_ns;
	if (window_ns < 5000000000ULL) {
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	uint64_t frames = g_frames;
	uint64_t cost_total = g_cost_total_ns;
	uint64_t cost_max = g_cost_max_ns;
	uint64_t bytes = g_bytes;
	int w = g_width, h = g_height;
	g_frames = 0;
	g_cost_total_ns = 0;
	g_cost_max_ns = 0;
	g_bytes = 0;
	g_last_log_ns = now;
	double cpu = g_cpu ? os_cpu_usage_info_query(g_cpu) : 0.0;
	pthread_mutex_unlock(&g_mutex);

	if (frames == 0)
		return; /* nothing streamed this window */

	double seconds = (double)window_ns / 1e9;
	double fps = (double)frames / seconds;
	double avg_ms = (double)cost_total / (double)frames / 1e6;
	double max_ms = (double)cost_max / 1e6;
	double mbs = (double)bytes / seconds / 1e6;

	blog(LOG_INFO,
	     "[lenslink][bench] pipeline=%s | %dx%d ~%.0f fps | "
	     "video-path cost/frame: avg %.2f ms, max %.2f ms | "
	     "pixel copies: %.1f MB/s | OBS process CPU: %.1f%%",
	     lenslink_settings_gpu_pipeline() ? "gpu" : "standard", w, h, fps,
	     avg_ms, max_ms, mbs, cpu);
}
