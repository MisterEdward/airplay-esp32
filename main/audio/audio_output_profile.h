#pragma once

#include <stddef.h>
#include <stdint.h>

/** Convert AirPlay dB to software Q15 using the selected hardware profile. */
int32_t audio_output_profile_volume_q15(float airplay_db);

/** Initialize the optional low-shelf tone correction. */
void audio_output_profile_init(uint32_t sample_rate);

/** Clear filter history at a stream discontinuity. */
void audio_output_profile_reset(void);

/** Apply the selected tone profile to interleaved stereo PCM. */
void audio_output_profile_process(int16_t *pcm, size_t frames);
