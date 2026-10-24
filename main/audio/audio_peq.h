#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Parametric EQ for the speaker: a measured room/speaker correction, not a
 * tone control.
 *
 * Each channel gets its own cascade of up to PEQ_MAX_BANDS biquads (RBJ
 * audio-EQ cookbook), designed from the filter list Room EQ Wizard (REW)
 * exports, so a measurement made with REW drops in unchanged.  A preamp is
 * folded in from the cascade's own peak gain so a boost can never clip.
 *
 * Runs in the render task on the block that leaves the resampler, before the
 * envelope applies volume and fades.  The filters are processed in float
 * (Direct Form I: the state is the real signal history, so a coefficient
 * change does not upset it), and the result is requantised to 16 bits with
 * TPDF dither.
 *
 * A new design never replaces the old one mid-signal: the render task picks
 * it up at a block boundary and crossfades the old cascade's output into
 * the new one over that block, so changing filters or bypassing them while
 * music plays makes no click.
 *
 * Pure C, no ESP-IDF: designed and processed on the host in
 * tests/host/test_peq.c.
 */

#define PEQ_MAX_BANDS     16
#define PEQ_CHANNELS      2
#define PEQ_MAX_PREAMP_DB 0.0f
#define PEQ_MIN_PREAMP_DB -24.0f

typedef enum {
  PEQ_OFF = 0,
  PEQ_PK, // peaking: fc, gain, q
  PEQ_LS, // low shelf: fc, gain, q (REW "LS"/"LSC"; q 0.707 if absent)
  PEQ_HS, // high shelf
  PEQ_LP, // 2nd-order low pass: fc, q
  PEQ_HP, // 2nd-order high pass: fc, q
  PEQ_TYPE_COUNT,
} peq_type_t;

typedef struct {
  uint8_t type; // peq_type_t
  float fc_hz;
  float gain_db;
  float q;
} peq_band_t;

typedef struct {
  uint8_t count;
  peq_band_t band[PEQ_MAX_BANDS];
  float preamp_db; // user preamp from the file (<= 0); auto preamp adds to it
} peq_channel_t;

/** The persisted configuration. */
typedef struct {
  uint32_t version; // PEQ_CONFIG_VERSION
  bool enabled;     // false = bypass
  peq_channel_t ch[PEQ_CHANNELS];
} peq_config_t;

#define PEQ_CONFIG_VERSION 1

typedef struct {
  float b0, b1, b2, a1, a2; // normalised by a0
} peq_biquad_t;

/** A designed cascade, ready for the render task. */
typedef struct {
  bool active; // false = identity (bypass, or nothing to do)
  uint8_t count[PEQ_CHANNELS];
  peq_biquad_t bq[PEQ_CHANNELS][PEQ_MAX_BANDS];
  float gain[PEQ_CHANNELS];      // linear preamp, folded in at the input
  float preamp_db[PEQ_CHANNELS]; // what was applied: user + auto
  float peak_db[PEQ_CHANNELS];   // cascade peak before the preamp
} peq_design_t;

typedef struct {
  float x1, x2, y1, y2;
} peq_state_t;

/** Render-side state, owned by the render task. */
#define PEQ_CHUNK 128

typedef struct {
  peq_design_t cur;
  peq_state_t st[PEQ_CHANNELS][PEQ_MAX_BANDS];
  uint32_t rng; // dither noise
  // Scratch, here rather than on the render task's stack.
  float in[PEQ_CHUNK], wet[PEQ_CHUNK], dry[PEQ_CHUNK];
} peq_runtime_t;

/** Fill a flat, enabled-but-empty configuration. */
void peq_config_default(peq_config_t *cfg);

/** Clamp every field into a range the designer accepts; drops OFF bands. */
void peq_config_sanitize(peq_config_t *cfg, uint32_t sample_rate);

/**
 * Design the cascade.  Preamp per channel = user preamp, lowered further if
 * the cascade's peak (evaluated on a dense log grid) would exceed 0 dBFS,
 * with 0.3 dB of margin for what the grid can miss.
 */
void peq_design(const peq_config_t *cfg, uint32_t sample_rate,
                peq_design_t *out);

/** Magnitude in dB of one channel of a design at f (includes the preamp). */
float peq_design_response_db(const peq_design_t *d, int ch, float f_hz,
                             uint32_t sample_rate);

/**
 * Parse a REW / Equalizer APO filter list into one channel.
 *
 * Accepts lines such as
 *   Filter  1: ON  PK       Fc   63.0 Hz  Gain  -5.0 dB  Q  4.00
 *   Filter 2: ON LSC Fc 105 Hz Gain 3.5 dB Q 0.71
 *   Filter 3: ON HS Fc 8000 Hz Gain 2 dB
 *   Filter 4: ON PK Fc 1000 Hz Gain -3 dB BW Oct 0.5
 *   Preamp: -4.2 dB
 * and ignores everything else (headers, "OFF"/"None" filters, comments).
 *
 * @param skipped  number of filter lines that were ON but not understood
 * @return number of bands parsed (0..PEQ_MAX_BANDS), or -1 if more than
 *         PEQ_MAX_BANDS active filters were present
 */
int peq_parse_rew(const char *text, size_t len, peq_channel_t *out,
                  int *skipped);

/** Short name for a type ("PK", "LS", ...), and the reverse. */
const char *peq_type_name(peq_type_t t);
peq_type_t peq_type_from_name(const char *name);

/** Initialise the runtime to identity. */
void peq_runtime_init(peq_runtime_t *rt);

/** Forget the signal history (flush, pause, source switch). */
void peq_runtime_reset(peq_runtime_t *rt);

/**
 * Process interleaved stereo int16 in place.
 *
 * @param next  a new design to switch to during this block (crossfaded from
 *              the current one), or NULL to keep the current one
 */
void peq_process(peq_runtime_t *rt, const peq_design_t *next, int16_t *pcm,
                 size_t frames);
