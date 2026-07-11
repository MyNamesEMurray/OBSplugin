#pragma once

#include <obs-module.h>
#include <stdint.h>
#include <stdbool.h>

struct h264_decoder;

struct h264_decoder *h264_decoder_create(void);
void h264_decoder_destroy(struct h264_decoder *dec);

/*
 * Decodes one Annex B access unit and pushes any resulting frames to the
 * source via obs_source_output_video(). Returns false on a hard decoder
 * error (caller should recreate the decoder and wait for a keyframe).
 */
bool h264_decoder_decode(struct h264_decoder *dec, obs_source_t *source,
			 const uint8_t *data, size_t size, uint64_t pts_ns);
