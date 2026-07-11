#include <obs-module.h>

#include "net-compat.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("ios-camera-source", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Use an iOS device's camera as a video source over the local "
	       "network (companion app required)";
}

extern struct obs_source_info ios_camera_source_info;

bool obs_module_load(void)
{
	if (!net_init()) {
		blog(LOG_ERROR, "[ios-camera] network init failed");
		return false;
	}

	obs_register_source(&ios_camera_source_info);
	blog(LOG_INFO, "[ios-camera] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	net_shutdown();
}
