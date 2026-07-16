#include <obs-module.h>

#include "net-compat.h"

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

	obs_register_source(&ios_camera_source_info);
	obs_register_source(&lenslink_screen_source_info);
#ifdef LENSLINK_FRONTEND
	lenslink_frontend_init();
#endif
	blog(LOG_INFO, "[lenslink] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
#ifdef LENSLINK_FRONTEND
	lenslink_frontend_shutdown();
#endif
	net_shutdown();
}
