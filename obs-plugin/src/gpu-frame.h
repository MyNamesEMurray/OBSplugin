#pragma once

#include <obs-module.h>
#include <stdbool.h>

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GPU-frame interop: turns a decoded *hardware* AVFrame into gs_texture(s)
 * without the pixels ever visiting system memory. One implementation per
 * platform behind #ifdefs:
 *
 *   Linux    VAAPI frame  -> DRM PRIME (dmabuf) -> EGLImage textures
 *   macOS    VideoToolbox -> IOSurface          -> GL rectangle texture
 *   Windows  D3D11VA      -> keyed-mutex shared NV12 texture pair
 *
 * Every path can fail at runtime (GLX instead of EGL, multi-GPU device
 * mismatch, driver quirks, an unexpected hw format): gpu_frame_map()
 * returns false and the caller falls back to the CPU pipeline for that
 * source. Failures are logged once per context, not per frame.
 *
 * Threading: map/unmap must be called inside the OBS graphics context
 * (i.e. from video_render). The mapped textures are valid only until
 * gpu_frame_unmap(); the AVFrame must stay referenced by the caller for
 * that same window.
 */

struct gpu_frame_ctx;

struct gpu_frame_map_result {
	/* rgba: tex[0] is a full-color texture (macOS). Otherwise NV12:
	 * tex[0] = luma (R8), tex[1] = chroma (R8G8, half size). */
	gs_texture_t *tex[2];
	bool rgba;
	uint32_t width, height;
};

struct gpu_frame_ctx *gpu_frame_ctx_create(void);
void gpu_frame_ctx_destroy(struct gpu_frame_ctx *ctx);

/* True if this platform build has any chance of mapping `frame`. Cheap;
 * callable from any thread. */
bool gpu_frame_supported(const AVFrame *frame);

/* Prepares textures for a NEW frame (import/rebind/copy). The result
 * stays valid for repeated draws until the next map or ctx destroy. */
bool gpu_frame_map(struct gpu_frame_ctx *ctx, AVFrame *frame,
		   struct gpu_frame_map_result *out);

/* Brackets each DRAW of the mapped textures (Windows: keyed-mutex
 * acquire/release; no-ops elsewhere). lock returns false = skip draw. */
bool gpu_frame_lock(struct gpu_frame_ctx *ctx);
void gpu_frame_unlock(struct gpu_frame_ctx *ctx);

#ifdef __cplusplus
}
#endif
