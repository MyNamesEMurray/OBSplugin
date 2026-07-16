#include "gpu-frame.h"

#include <util/bmem.h>

#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>

/* ------------------------------------------------------------------ */
/* Linux: VAAPI -> DRM PRIME (dmabuf) -> EGLImage textures             */
/* ------------------------------------------------------------------ */
#if defined(__linux__)

#include <libavutil/hwcontext_drm.h>
#include <unistd.h>

#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8 0x20203852
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88 0x38385247
#endif

struct gpu_frame_ctx {
	gs_texture_t *tex[2];
	AVFrame *drm; /* the PRIME mapping keeps the dmabuf fds alive */
	bool warned;
	bool announced;
};

struct gpu_frame_ctx *gpu_frame_ctx_create(void)
{
	return bzalloc(sizeof(struct gpu_frame_ctx));
}

static void linux_release(struct gpu_frame_ctx *ctx)
{
	for (int i = 0; i < 2; i++) {
		if (ctx->tex[i]) {
			gs_texture_destroy(ctx->tex[i]);
			ctx->tex[i] = NULL;
		}
	}
	if (ctx->drm) {
		av_frame_free(&ctx->drm);
	}
}

void gpu_frame_ctx_destroy(struct gpu_frame_ctx *ctx)
{
	if (!ctx)
		return;
	linux_release(ctx);
	bfree(ctx);
}

bool gpu_frame_supported(const AVFrame *frame)
{
	return frame && frame->format == AV_PIX_FMT_VAAPI;
}

bool gpu_frame_map(struct gpu_frame_ctx *ctx, AVFrame *frame,
		   struct gpu_frame_map_result *out)
{
	if (!gpu_frame_supported(frame))
		return false;

	AVFrame *drm = av_frame_alloc();
	if (!drm)
		return false;
	drm->format = AV_PIX_FMT_DRM_PRIME;
	if (av_hwframe_map(drm, frame, AV_HWFRAME_MAP_READ) < 0) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: VAAPI frame cannot be "
			     "exported as dmabuf — using the CPU path");
		}
		av_frame_free(&drm);
		return false;
	}

	const AVDRMFrameDescriptor *desc =
		(const AVDRMFrameDescriptor *)drm->data[0];
	/* NV12 as one or two objects; layers describe the two planes. */
	if (desc->nb_layers < 2 && !(desc->nb_layers == 1 &&
				     desc->layers[0].nb_planes >= 2)) {
		av_frame_free(&drm);
		return false;
	}

	uint32_t w = (uint32_t)frame->width, h = (uint32_t)frame->height;

	int fds[2];
	uint32_t strides[2], offsets[2];
	uint64_t modifiers[2];
	/* Plane i: layer i (two layers) or plane i of layer 0. */
	for (int i = 0; i < 2; i++) {
		const AVDRMLayerDescriptor *layer =
			desc->nb_layers >= 2 ? &desc->layers[i]
					     : &desc->layers[0];
		const AVDRMPlaneDescriptor *plane =
			desc->nb_layers >= 2 ? &layer->planes[0]
					     : &layer->planes[i];
		const AVDRMObjectDescriptor *obj =
			&desc->objects[plane->object_index];
		fds[i] = obj->fd;
		strides[i] = (uint32_t)plane->pitch;
		offsets[i] = (uint32_t)plane->offset;
		modifiers[i] = obj->format_modifier;
	}

	gs_texture_t *y = gs_texture_create_from_dmabuf(
		w, h, DRM_FORMAT_R8, GS_R8, 1, &fds[0], &strides[0],
		&offsets[0], &modifiers[0]);
	gs_texture_t *uv = gs_texture_create_from_dmabuf(
		w / 2, h / 2, DRM_FORMAT_GR88, GS_R8G8, 1, &fds[1],
		&strides[1], &offsets[1], &modifiers[1]);
	if (!y || !uv) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: dmabuf import failed "
			     "(OBS not on EGL, or a GPU mismatch) — using "
			     "the CPU path");
		}
		if (y)
			gs_texture_destroy(y);
		if (uv)
			gs_texture_destroy(uv);
		av_frame_free(&drm);
		return false;
	}

	linux_release(ctx);
	ctx->tex[0] = y;
	ctx->tex[1] = uv;
	ctx->drm = drm;

	if (!ctx->announced) {
		ctx->announced = true;
		blog(LOG_INFO, "[lenslink] gpu pipeline: zero-copy active "
			       "(VAAPI dmabuf -> EGL)");
	}

	out->tex[0] = y;
	out->tex[1] = uv;
	out->rgba = false;
	out->width = w;
	out->height = h;
	return true;
}

bool gpu_frame_lock(struct gpu_frame_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);
	return true;
}

void gpu_frame_unlock(struct gpu_frame_ctx *ctx)
{
	/* Textures/mapping stay alive until the next map (the compositor
	 * may redraw with the same frame); release happens there. */
	UNUSED_PARAMETER(ctx);
}

/* ------------------------------------------------------------------ */
/* macOS: VideoToolbox -> IOSurface -> GL rectangle texture            */
/* ------------------------------------------------------------------ */
#elif defined(__APPLE__)

#include <libavutil/hwcontext_videotoolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

struct gpu_frame_ctx {
	gs_texture_t *tex;
	bool warned;
	bool announced;
};

struct gpu_frame_ctx *gpu_frame_ctx_create(void)
{
	return bzalloc(sizeof(struct gpu_frame_ctx));
}

void gpu_frame_ctx_destroy(struct gpu_frame_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->tex)
		gs_texture_destroy(ctx->tex);
	bfree(ctx);
}

bool gpu_frame_supported(const AVFrame *frame)
{
	/* Only BGRA surfaces map to a single OBS texture; the decoder
	 * requests BGRA output in GPU mode (see h264-decoder.c). NV12
	 * surfaces fall back to the CPU path. */
	if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX)
		return false;
	CVPixelBufferRef pix = (CVPixelBufferRef)frame->data[3];
	return pix && CVPixelBufferGetPixelFormatType(pix) ==
			      kCVPixelFormatType_32BGRA;
}

bool gpu_frame_map(struct gpu_frame_ctx *ctx, AVFrame *frame,
		   struct gpu_frame_map_result *out)
{
	if (!gpu_frame_supported(frame))
		return false;

	CVPixelBufferRef pix = (CVPixelBufferRef)frame->data[3];
	IOSurfaceRef surf = CVPixelBufferGetIOSurface(pix);
	if (!surf) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: decoded buffer has "
			     "no IOSurface — using the CPU path");
		}
		return false;
	}

	/* Rebind when possible: creating a texture per frame would leak
	 * GL names at 60 fps. */
	if (ctx->tex) {
		if (!gs_texture_rebind_iosurface(ctx->tex, surf)) {
			gs_texture_destroy(ctx->tex);
			ctx->tex = NULL;
		}
	}
	if (!ctx->tex) {
		ctx->tex = gs_texture_create_from_iosurface(surf);
		if (!ctx->tex) {
			if (!ctx->warned) {
				ctx->warned = true;
				blog(LOG_WARNING,
				     "[lenslink] gpu pipeline: IOSurface "
				     "texture creation failed — using the "
				     "CPU path");
			}
			return false;
		}
	}

	if (!ctx->announced) {
		ctx->announced = true;
		blog(LOG_INFO, "[lenslink] gpu pipeline: zero-copy active "
			       "(VideoToolbox IOSurface)");
	}

	out->tex[0] = ctx->tex;
	out->tex[1] = NULL;
	out->rgba = true;
	out->width = (uint32_t)frame->width;
	out->height = (uint32_t)frame->height;
	return true;
}

bool gpu_frame_lock(struct gpu_frame_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);
	return true;
}

void gpu_frame_unlock(struct gpu_frame_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);
}

/* ------------------------------------------------------------------ */
/* Windows: D3D11VA -> keyed-mutex shared NV12 texture pair            */
/* ------------------------------------------------------------------ */
#elif defined(_WIN32)

#define COBJMACROS
#include <libavutil/hwcontext_d3d11va.h>
#include <d3d11.h>

struct gpu_frame_ctx {
	/* OBS-side pair backed by ONE shared NV12 texture (keyed mutex). */
	gs_texture_t *tex_y;
	gs_texture_t *tex_uv;
	uint32_t width, height;
	uint32_t shared_handle;

	/* FFmpeg-device-side view of the same texture. */
	ID3D11Texture2D *ff_shared;
	IDXGIKeyedMutex *ff_mutex;
	ID3D11Device *ff_device; /* identifies the cache; not owned */

	bool acquired; /* OBS-side keyed mutex held between map/unmap */
	bool warned;
	bool announced;
};

struct gpu_frame_ctx *gpu_frame_ctx_create(void)
{
	return bzalloc(sizeof(struct gpu_frame_ctx));
}

static void win_release(struct gpu_frame_ctx *ctx)
{
	if (ctx->ff_mutex) {
		IDXGIKeyedMutex_Release(ctx->ff_mutex);
		ctx->ff_mutex = NULL;
	}
	if (ctx->ff_shared) {
		ID3D11Texture2D_Release(ctx->ff_shared);
		ctx->ff_shared = NULL;
	}
	if (ctx->tex_y) {
		gs_texture_destroy(ctx->tex_y);
		ctx->tex_y = NULL;
	}
	/* tex_uv shares the underlying object with tex_y; destroying it
	 * separately is still required (it is its own gs_texture). */
	if (ctx->tex_uv) {
		gs_texture_destroy(ctx->tex_uv);
		ctx->tex_uv = NULL;
	}
	ctx->ff_device = NULL;
	ctx->shared_handle = 0;
	ctx->width = ctx->height = 0;
}

void gpu_frame_ctx_destroy(struct gpu_frame_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->acquired && ctx->tex_y)
		gs_texture_release_sync(ctx->tex_y, 0);
	win_release(ctx);
	bfree(ctx);
}

bool gpu_frame_supported(const AVFrame *frame)
{
	/* DXVA2 frames are D3D9 surfaces — no D3D11 interop; the decoder's
	 * hardware priority list tries D3D11VA first anyway. */
	return frame && frame->format == AV_PIX_FMT_D3D11;
}

static bool win_ensure_textures(struct gpu_frame_ctx *ctx, uint32_t w,
				uint32_t h, ID3D11Device *ff_device)
{
	if (ctx->tex_y && ctx->width == w && ctx->height == h &&
	    ctx->ff_device == ff_device)
		return true;

	win_release(ctx);

	/* OBS side: an NV12 texture pair (one underlying resource, a luma
	 * view and a chroma view) created shared with a keyed mutex. */
	if (!gs_texture_create_nv12(&ctx->tex_y, &ctx->tex_uv, w, h,
				    GS_SHARED_KM_TEX)) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: shared NV12 texture "
			     "creation failed — using the CPU path");
		}
		return false;
	}
	/* libobs creates KM textures with key 0 already acquired on the
	 * OBS device; without releasing that initial hold the decode
	 * device's AcquireSync below can never succeed. */
	gs_texture_release_sync(ctx->tex_y, 0);

	ctx->shared_handle = gs_texture_get_shared_handle(ctx->tex_y);
	if (ctx->shared_handle == GS_INVALID_HANDLE) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: shared NV12 texture "
			     "has no shared handle — using the CPU path");
		}
		win_release(ctx);
		return false;
	}

	/* FFmpeg-device side: open the same resource for the copy. */
	HRESULT hr = ID3D11Device_OpenSharedResource(
		ff_device, (HANDLE)(uintptr_t)ctx->shared_handle,
		&IID_ID3D11Texture2D, (void **)&ctx->ff_shared);
	if (FAILED(hr)) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: OpenSharedResource "
			     "failed (0x%lx, GPU mismatch?) — using the CPU "
			     "path",
			     (unsigned long)hr);
		}
		win_release(ctx);
		return false;
	}
	hr = ID3D11Texture2D_QueryInterface(ctx->ff_shared,
					    &IID_IDXGIKeyedMutex,
					    (void **)&ctx->ff_mutex);
	if (FAILED(hr)) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: shared texture has no "
			     "keyed mutex (0x%lx) — using the CPU path",
			     (unsigned long)hr);
		}
		win_release(ctx);
		return false;
	}

	ctx->ff_device = ff_device;
	ctx->width = w;
	ctx->height = h;
	return true;
}

bool gpu_frame_map(struct gpu_frame_ctx *ctx, AVFrame *frame,
		   struct gpu_frame_map_result *out)
{
	if (!gpu_frame_supported(frame) || !frame->hw_frames_ctx)
		return false;

	AVHWFramesContext *frames =
		(AVHWFramesContext *)frame->hw_frames_ctx->data;
	AVHWDeviceContext *devctx = frames->device_ctx;
	AVD3D11VADeviceContext *d3d = devctx->hwctx;

	ID3D11Texture2D *decoded = (ID3D11Texture2D *)frame->data[0];
	UINT slice = (UINT)(uintptr_t)frame->data[1];

	if (!win_ensure_textures(ctx, (uint32_t)frame->width,
				 (uint32_t)frame->height, d3d->device))
		return false;

	/* Copy the decoder's array slice into the shared texture on the
	 * FFmpeg device. The hwctx lock serializes us against the decoder
	 * worker, which uses the same immediate context. */
	if (IDXGIKeyedMutex_AcquireSync(ctx->ff_mutex, 0, 33) != S_OK) {
		if (!ctx->warned) {
			ctx->warned = true;
			blog(LOG_WARNING,
			     "[lenslink] gpu pipeline: keyed-mutex acquire "
			     "timed out on the decode device — using the CPU "
			     "path");
		}
		return false;
	}
	d3d->lock(d3d->lock_ctx);
	D3D11_BOX box = {0, 0, 0, (UINT)frame->width, (UINT)frame->height,
			 1};
	ID3D11DeviceContext_CopySubresourceRegion(
		d3d->device_context, (ID3D11Resource *)ctx->ff_shared, 0, 0,
		0, 0, (ID3D11Resource *)decoded, slice, &box);
	/* Flush BEFORE releasing the mutex: the copy sits in this device's
	 * command buffer until submitted, and the keyed-mutex release does
	 * not submit it. Without this, OBS acquires the mutex and draws a
	 * texture the copy hasn't reached — a silently black source. */
	ID3D11DeviceContext_Flush(d3d->device_context);
	d3d->unlock(d3d->lock_ctx);
	IDXGIKeyedMutex_ReleaseSync(ctx->ff_mutex, 0);

	if (!ctx->announced) {
		ctx->announced = true;
		blog(LOG_INFO, "[lenslink] gpu pipeline: zero-copy active "
			       "(D3D11 shared NV12)");
	}

	out->tex[0] = ctx->tex_y;
	out->tex[1] = ctx->tex_uv;
	out->rgba = false;
	out->width = ctx->width;
	out->height = ctx->height;
	return true;
}

bool gpu_frame_lock(struct gpu_frame_ctx *ctx)
{
	if (!ctx->tex_y)
		return false;
	if (gs_texture_acquire_sync(ctx->tex_y, 0, 33) != 0)
		return false;
	ctx->acquired = true;
	return true;
}

void gpu_frame_unlock(struct gpu_frame_ctx *ctx)
{
	if (ctx->acquired && ctx->tex_y) {
		gs_texture_release_sync(ctx->tex_y, 0);
		ctx->acquired = false;
	}
}

/* ------------------------------------------------------------------ */
#else

struct gpu_frame_ctx {
	int unused;
};

struct gpu_frame_ctx *gpu_frame_ctx_create(void)
{
	return bzalloc(sizeof(struct gpu_frame_ctx));
}

void gpu_frame_ctx_destroy(struct gpu_frame_ctx *ctx)
{
	bfree(ctx);
}

bool gpu_frame_supported(const AVFrame *frame)
{
	UNUSED_PARAMETER(frame);
	return false;
}

bool gpu_frame_map(struct gpu_frame_ctx *ctx, AVFrame *frame,
		   struct gpu_frame_map_result *out)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(frame);
	UNUSED_PARAMETER(out);
	return false;
}

bool gpu_frame_lock(struct gpu_frame_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);
	return false;
}

void gpu_frame_unlock(struct gpu_frame_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);
}

#endif
