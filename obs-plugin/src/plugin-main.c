#include <obs-module.h>

#include "net-compat.h"
#include "plugin-settings.h"
#include "pipeline-bench.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("lenslink", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "LensLink — use an iPhone or iPad camera, or mirror its "
	       "screen, as a video source over Wi-Fi or USB (LensLink "
	       "companion app required)";
}

extern struct obs_source_info ios_camera_source_info;
extern struct obs_source_info lenslink_screen_source_info;

/* ios-camera-source.c: patches a source info for the GPU (beta)
 * pipeline — self-rendering sync source instead of async pixel-pusher.
 * Must run before registration, which is why the pipeline choice only
 * changes on OBS restart. */
extern void lenslink_sources_use_gpu_pipeline(struct obs_source_info *info);

#ifdef LENSLINK_FRONTEND
/* frontend-ui.cpp — status-bar readout and dock. Compiled in only when
 * Qt6 is available; degrades to nothing at runtime in frontend-less OBS. */
extern void lenslink_frontend_init(void);
extern void lenslink_frontend_shutdown(void);
#endif

bool obs_module_load(void)
{
	if (!net_init()) {
		blog(LOG_ERROR, "[lenslink] network init failed");
		return false;
	}

	lenslink_settings_init();
	if (lenslink_settings_gpu_pipeline()) {
		lenslink_sources_use_gpu_pipeline(&ios_camera_source_info);
		lenslink_sources_use_gpu_pipeline(&lenslink_screen_source_info);
	}

	obs_register_source(&ios_camera_source_info);
	obs_register_source(&lenslink_screen_source_info);
#ifdef LENSLINK_FRONTEND
	lenslink_frontend_init();
#endif
	blog(LOG_INFO, "[lenslink] LensLink %s loaded", LENSLINK_VERSION);
	return true;
}

void obs_module_unload(void)
{
#ifdef LENSLINK_FRONTEND
	lenslink_frontend_shutdown();
#endif
	lenslink_bench_shutdown();
	lenslink_settings_shutdown();
	net_shutdown();
}
