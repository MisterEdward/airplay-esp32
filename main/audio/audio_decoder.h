#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"

typedef struct audio_decoder audio_decoder_t;

typedef struct {
  audio_format_t format;
} audio_decoder_config_t;

typedef struct {
  int channels;
} audio_decode_info_t;

audio_decoder_t *audio_decoder_create(const audio_decoder_config_t *config);
void audio_decoder_destroy(audio_decoder_t *decoder);
int audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                         size_t input_len, int16_t *output,
                         size_t output_capacity_samples,
                         audio_decode_info_t *info);

bool audio_decoder_is_aac(const audio_decoder_t *decoder);

/**
 * Diagnostic: reopen the AAC decoder before the next frame it decodes, as the
 * corrupt-frame path does.  Safe from any task.  Used to tell a decoder fault
 * from a sender fault when the bass disappears mid-stream.
 */
void audio_decoder_request_reset(void);
bool audio_decoder_is_alac(const audio_decoder_t *decoder);
