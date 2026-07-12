#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <stdbool.h>

#include <libavcodec/avcodec.h>

struct h264_decoder;

/*
 * codec_id: AV_CODEC_ID_H264 or AV_CODEC_ID_HEVC.
 * allow_hw: try GPU decoding (D3D11VA/VideoToolbox/VAAPI), silently
 * falling back to software when unavailable.
 */
struct h264_decoder *h264_decoder_create(enum AVCodecID codec_id,
					 bool allow_hw);

/* Whether this instance actually decodes on the GPU. */
bool h264_decoder_is_hw(const struct h264_decoder *dec);
void h264_decoder_destroy(struct h264_decoder *dec);

/*
 * Decodes one Annex B access unit and pushes any resulting frames to the
 * source via obs_source_output_video(). Returns false on a hard decoder
 * error (caller should recreate the decoder and wait for a keyframe).
 */
bool h264_decoder_decode(struct h264_decoder *dec, obs_source_t *source,
			 const uint8_t *data, size_t size, uint64_t pts_ns);
