#include "h264-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>

#include <media-io/video-io.h>
#include <util/platform.h>

struct h264_decoder {
	AVCodecContext *ctx;
	AVPacket *pkt;
	AVFrame *frame;
};

struct h264_decoder *h264_decoder_create(void)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		blog(LOG_ERROR, "[ios-camera] H.264 decoder not available");
		return NULL;
	}

	struct h264_decoder *dec = bzalloc(sizeof(*dec));

	dec->ctx = avcodec_alloc_context3(codec);
	dec->pkt = av_packet_alloc();
	dec->frame = av_frame_alloc();
	if (!dec->ctx || !dec->pkt || !dec->frame)
		goto fail;

	dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	dec->ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
	/* Frame threading buffers one input frame per thread before any
	 * output appears — pure added latency for a live stream. The
	 * encoder sends single-slice frames, so decode single-threaded. */
	dec->ctx->thread_count = 1;

	if (avcodec_open2(dec->ctx, codec, NULL) < 0) {
		blog(LOG_ERROR, "[ios-camera] failed to open H.264 decoder");
		goto fail;
	}

	return dec;

fail:
	h264_decoder_destroy(dec);
	return NULL;
}

void h264_decoder_destroy(struct h264_decoder *dec)
{
	if (!dec)
		return;

	avcodec_free_context(&dec->ctx);
	av_packet_free(&dec->pkt);
	av_frame_free(&dec->frame);
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

		struct obs_source_frame out = {0};
		if (!avframe_to_obs(dec->frame, &out)) {
			blog(LOG_WARNING,
			     "[ios-camera] unsupported pixel format %d",
			     dec->frame->format);
			av_frame_unref(dec->frame);
			continue;
		}

		out.timestamp = dec->frame->pts != AV_NOPTS_VALUE
					? (uint64_t)dec->frame->pts
					: os_gettime_ns();

		obs_source_output_video(source, &out);
		av_frame_unref(dec->frame);
	}

	return true;
}
