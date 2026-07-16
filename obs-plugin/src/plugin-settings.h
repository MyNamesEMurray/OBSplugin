#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugin-wide settings — things that don't make sense per source: the
 * decode pipeline choice (fixed at module load), the web control panel
 * (one port for the whole plugin), and the diagnostics switches.
 *
 * Stored as JSON in the plugin's config directory (settings.json).
 * Edited from the Tools → "LensLink Settings" dialog when the Qt
 * frontend module is built; hand-editable otherwise.
 *
 * Getters are safe from any thread. Values change only via
 * lenslink_settings_update() (dialog Apply/OK); torn reads are benign
 * (single bools/int, consumers re-read every use).
 */

void lenslink_settings_init(void);
void lenslink_settings_shutdown(void);

/* GPU (beta) pipeline: decoded frames stay on the GPU and the source
 * renders textures instead of pushing pixels. Read ONCE at module load —
 * it selects how the sources are registered, so changes need an OBS
 * restart (the dialog says so). */
bool lenslink_settings_gpu_pipeline(void);

bool lenslink_settings_web_enabled(void);
int lenslink_settings_web_port(void);
bool lenslink_settings_diagnostics(void);
bool lenslink_settings_dump_stream(void);
/* Pipeline benchmark logging (see pipeline-bench.h). */
bool lenslink_settings_benchmark(void);

/* Applies (and persists) new values from the settings dialog. Web/
 * diagnostics changes take effect within ~1 s (sources poll on their
 * maintenance tick); the pipeline choice applies on restart. */
struct obs_data;
void lenslink_settings_update(struct obs_data *values);

/* A copy of the current values for the dialog to edit (caller releases). */
struct obs_data *lenslink_settings_snapshot(void);

/* Implemented in ios-camera-source.c: pushes freshly saved plugin-wide
 * settings into every live source (web server restart/rebind, diagnostic
 * flags). Call after lenslink_settings_update(); UI thread. */
void ios_camera_apply_global_settings(void);

/* Setting keys (shared with the dialog). */
#define LLS_GPU_PIPELINE "gpu_pipeline"
#define LLS_WEB_ENABLED "web_control"
#define LLS_WEB_PORT "web_control_port"
#define LLS_DIAGNOSTICS "diagnostics"
#define LLS_DUMP_STREAM "dump_stream"
#define LLS_BENCHMARK "pipeline_benchmark"

#define LLS_WEB_PORT_DEFAULT 9980

#ifdef __cplusplus
}
#endif
