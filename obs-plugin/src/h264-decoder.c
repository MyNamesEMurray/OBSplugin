#include "h264-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>

#include <media-io/video-io.h>
#include <util/platform.h>

struct h264_decoder {
	AVCodecContext *ctx;
	AVPacket *pkt;
	AVFrame *frame;
	AVFrame *sw_frame; /* CPU copy of a GPU-decoded frame */

	AVBufferRef *hw_device;
	enum AVPixelFormat hw_pix_fmt;
	bool is_hw;
};

/* GPU decode APIs to try, best-first per platform. */
static const enum AVHWDeviceType hw_priority[] = {
#ifdef _WIN32
	AV_HWDEVICE_TYPE_D3D11VA,
	AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
	AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_VDPAU,
#endif
	AV_HWDEVICE_TYPE_NONE,
};

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
					const enum AVPixelFormat *formats)
{
	struct h264_decoder *dec = ctx->opaque;

	for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == dec->hw_pix_fmt)
			return *p;
	}

	/* Driver refused the hw format for this stream; take the first
	 * (software) format the decoder offers instead. */
	blog(LOG_WARNING,
	     "[ios-camera] hardware pixel format unavailable, "
	     "decoding in software");
	dec->is_hw = false;
	return formats[0];
}

static bool try_init_hw(struct h264_decoder *dec, const AVCodec *codec)
{
	for (int i = 0; hw_priority[i] != AV_HWDEVICE_TYPE_NONE; i++) {
		enum AVHWDeviceType type = hw_priority[i];

		enum AVPixelFormat pix = AV_PIX_FMT_NONE;
		for (int j = 0;; j++) {
			const AVCodecHWConfig *config =
				avcodec_get_hw_config(codec, j);
			if (!config)
				break;
			if ((config->methods &
			     AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
			    config->device_type == type) {
				pix = config->pix_fmt;
				break;
			}
		}
		if (pix == AV_PIX_FMT_NONE)
			continue;

		AVBufferRef *device = NULL;
		if (av_hwdevice_ctx_create(&device, type, NULL, NULL, 0) < 0)
			continue;

		dec->hw_device = device;
		dec->hw_pix_fmt = pix;
		dec->is_hw = true;
		dec->ctx->hw_device_ctx = av_buffer_ref(device);
		dec->ctx->opaque = dec;
		dec->ctx->get_format = get_hw_format;

		blog(LOG_INFO, "[ios-camera] hardware decoding via %s",
		     av_hwdevice_get_type_name(type));
		return true;
	}

	return false;
}

struct h264_decoder *h264_decoder_create(enum AVCodecID codec_id, bool allow_hw)
{
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		blog(LOG_ERROR, "[ios-camera] decoder for codec %d unavailable",
		     (int)codec_id);
		return NULL;
	}

	struct h264_decoder *dec = bzalloc(sizeof(*dec));

	dec->ctx = avcodec_alloc_context3(codec);
	dec->pkt = av_packet_alloc();
	dec->frame = av_frame_alloc();
	dec->sw_frame = av_frame_alloc();
	if (!dec->ctx || !dec->pkt || !dec->frame || !dec->sw_frame)
		goto fail;

	dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	dec->ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
	/* Frame threading buffers one input frame per thread before any
	 * output appears — pure added latency for a live stream. The
	 * encoder sends single-slice frames, so decode single-threaded. */
	dec->ctx->thread_count = 1;

	if (allow_hw && !try_init_hw(dec, codec))
		blog(LOG_INFO,
		     "[ios-camera] no hardware decoder available, "
		     "using software");

	if (avcodec_open2(dec->ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[ios-camera] failed to open decoder");
		goto fail;
	}

	return dec;

fail:
	h264_decoder_destroy(dec);
	return NULL;
}

bool h264_decoder_is_hw(const struct h264_decoder *dec)
{
	return dec && dec->is_hw;
}

void h264_decoder_destroy(struct h264_decoder *dec)
{
	if (!dec)
		return;

	avcodec_free_context(&dec->ctx);
	av_packet_free(&dec->pkt);
	av_frame_free(&dec->frame);
	av_frame_free(&dec->sw_frame);
	av_buffer_unref(&dec->hw_device);
	bfree(dec);
}

static bool avframe_to_obs(const AVFrame *frame, struct obs_source_frame *out)
{
	switch (frame->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		out->format = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_NV12:
		out->format = VIDEO_FORMAT_NV12;
		break;
	case AV_PIX_FMT_YUV422P:
		out->format = VIDEO_FORMAT_I422;
		break;
	case AV_PIX_FMT_P010:
		out->format = VIDEO_FORMAT_P010;
		break;
	default:
		return false;
	}

	out->width = (uint32_t)frame->width;
	out->height = (uint32_t)frame->height;

	for (int i = 0; i < AV_NUM_DATA_POINTERS && frame->data[i]; i++) {
		out->data[i] = frame->data[i];
		out->linesize[i] = (uint32_t)frame->linesize[i];
	}

	bool full = frame->color_range == AVCOL_RANGE_JPEG ||
		    frame->format == AV_PIX_FMT_YUVJ420P;
	enum video_range_type range =
		full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	enum video_colorspace cs = frame->height > 720 ? VIDEO_CS_709
						       : VIDEO_CS_601;
	if (frame->colorspace == AVCOL_SPC_BT709)
		cs = VIDEO_CS_709;
	else if (frame->colorspace == AVCOL_SPC_SMPTE170M ||
		 frame->colorspace == AVCOL_SPC_BT470BG)
		cs = VIDEO_CS_601;

	out->full_range = range == VIDEO_RANGE_FULL;
	video_format_get_parameters(cs, range, out->color_matrix,
				    out->color_range_min,
				    out->color_range_max);
	return true;
}

bool h264_decoder_decode(struct h264_decoder *dec, obs_source_t *source,
			 const uint8_t *data, size_t size, uint64_t pts_ns)
{
	dec->pkt->data = (uint8_t *)data;
	dec->pkt->size = (int)size;
	dec->pkt->pts = (int64_t)pts_ns;
	dec->pkt->dts = (int64_t)pts_ns;

	int ret = avcodec_send_packet(dec->ctx, dec->pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN)) {
		blog(LOG_WARNING, "[ios-camera] avcodec_send_packet: %d", ret);
		return ret != AVERROR_INVALIDDATA ? false : true;
	}

	for (;;) {
		ret = avcodec_receive_frame(dec->ctx, dec->frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) {
			blog(LOG_WARNING,
			     "[ios-camera] avcodec_receive_frame: %d", ret);
			return false;
		}

		const AVFrame *out_frame = dec->frame;

		/* GPU-decoded frames live in GPU memory; copy to system
		 * memory for obs_source_output_video. */
		if (dec->hw_device && dec->frame->format == dec->hw_pix_fmt) {
			av_frame_unref(dec->sw_frame);
			if (av_hwframe_transfer_data(dec->sw_frame, dec->frame,
						     0) < 0) {
				blog(LOG_WARNING,
				     "[ios-camera] GPU frame download failed");
				av_frame_unref(dec->frame);
				return false;
			}
			av_frame_copy_props(dec->sw_frame, dec->frame);
			out_frame = dec->sw_frame;
		}

		struct obs_source_frame out = {0};
		if (!avframe_to_obs(out_frame, &out)) {
			blog(LOG_WARNING,
			     "[ios-camera] unsupported pixel format %d",
			     out_frame->format);
			av_frame_unref(dec->frame);
			continue;
		}

		out.timestamp = out_frame->pts != AV_NOPTS_VALUE
					? (uint64_t)out_frame->pts
					: os_gettime_ns();

		obs_source_output_video(source, &out);
		av_frame_unref(dec->frame);
	}

	return true;
}
