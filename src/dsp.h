#ifndef __DSP_H__
#define __DSP_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Per speaker audio processing: 5 band EQ, preamp, crossover (high and low
 * pass, Linkwitz-Riley 24 dB/oct) and channel routing. Profiles are stored per
 * output device, e.g. a HomePod pair playing only the bass (low pass) and two
 * minis as side speakers (high pass, one playing left, the other right).
 */

#define DSP_BANDS 5

enum dsp_channels
{
  DSP_CHANNELS_STEREO = 0,
  DSP_CHANNELS_LEFT,   // Left channel on both outputs
  DSP_CHANNELS_RIGHT,  // Right channel on both outputs
  DSP_CHANNELS_MONO,   // (L+R)/2 on both outputs
  DSP_CHANNELS_SWAP,
};

struct dsp_profile
{
  bool enabled;
  float preamp_db;           // -24 to +12
  float band_db[DSP_BANDS];  // -12 to +12, see dsp_band_freq
  int highpass_hz;           // 0 = off, else 20 to 20000
  int lowpass_hz;            // 0 = off, else 20 to 20000
  enum dsp_channels channels;
};

// Centre/corner frequencies of the bands: low shelf, 3 x peaking, high shelf
extern const int dsp_band_freq[DSP_BANDS];

struct dsp_ctx;

void
dsp_profile_default(struct dsp_profile *p);

// True if the profile does nothing to the audio (disabled or all neutral)
bool
dsp_profile_is_neutral(const struct dsp_profile *p);

bool
dsp_profile_equal(const struct dsp_profile *a, const struct dsp_profile *b);

// Returns 0 on success, -1 if the JSON is invalid (p is then left untouched).
// Values out of range are clamped.
int
dsp_profile_from_json(struct dsp_profile *p, const char *json);

// Returned string must be freed
char *
dsp_profile_to_json(const struct dsp_profile *p);

const char *
dsp_channels_to_string(enum dsp_channels channels);


/* ------------------------------ Processing ------------------------------- */

struct dsp_ctx *
dsp_new(const struct dsp_profile *p, int sample_rate);

// Changes the parameters, keeps the filter state so there is no click
void
dsp_update(struct dsp_ctx *ctx, const struct dsp_profile *p);

void
dsp_free(struct dsp_ctx *ctx);

// Processes interleaved 16 bit samples in place
void
dsp_process_s16(struct dsp_ctx *ctx, int16_t *samples, size_t frames, int channels);

// Number of samples that had to be clipped since the last call
uint64_t
dsp_clips_take(struct dsp_ctx *ctx);


/* ------------------------- Storage (per device) -------------------------- */
/*            Thread: any thread that has database access                     */

// Returns the device's profile (the default if none was saved)
void
dsp_profile_get(struct dsp_profile *p, uint64_t device_id);

int
dsp_profile_set(const struct dsp_profile *p, uint64_t device_id);

// Incremented on every dsp_profile_set(), so users can cheaply check for
// changes
uint32_t
dsp_generation_get(void);

#endif /* !__DSP_H__ */
