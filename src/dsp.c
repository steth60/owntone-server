/*
 * Copyright (C) 2026 Stephan Stickley <stephan.stickley@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Per speaker EQ, crossover and channel routing.
 *
 * Filters are second order IIR sections (biquads) with coefficients from
 * Robert Bristow-Johnson's "Audio EQ Cookbook", run in transposed direct form
 * II with double precision state. The crossover filters are Linkwitz-Riley
 * 4th order (two cascaded Butterworth sections), so a low pass on one speaker
 * and a high pass at the same frequency on another sum to a flat response.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>

#include <json.h>

#include "dsp.h"
#include "db.h"
#include "logger.h"
#include "misc.h"

#define DSP_MAX_CHANNELS   2
// Shelves and crossover sections per channel: 5 bands + 2 x 2 crossover
#define DSP_MAX_SECTIONS   (DSP_BANDS + 4)
#define DSP_PEAK_Q         0.9
#define DSP_BUTTERWORTH_Q  0.70710678118654752440
#define DSP_CACHE_SIZE     64
#define DSP_ADMIN_KEY_FMT  "dsp_profile_%" PRIu64

const int dsp_band_freq[DSP_BANDS] = { 60, 230, 910, 3600, 14000 };

static const char *dsp_channels_str[] = { "stereo", "left", "right", "mono", "swap" };

struct biquad
{
  double b0, b1, b2, a1, a2;
};

struct dsp_ctx
{
  struct dsp_profile profile;
  int sample_rate;
  double preamp;

  int nsections;
  struct biquad sections[DSP_MAX_SECTIONS];
  // Transposed direct form II state per channel and section
  double z1[DSP_MAX_CHANNELS][DSP_MAX_SECTIONS];
  double z2[DSP_MAX_CHANNELS][DSP_MAX_SECTIONS];

  uint64_t clips;
};

struct dsp_cache_entry
{
  bool in_use;
  uint64_t device_id;
  struct dsp_profile profile;
};

static pthread_mutex_t dsp_cache_lck = PTHREAD_MUTEX_INITIALIZER;
static struct dsp_cache_entry dsp_cache[DSP_CACHE_SIZE];
static uint32_t dsp_generation;


/* ------------------------------- Profiles ---------------------------------- */

void
dsp_profile_default(struct dsp_profile *p)
{
  memset(p, 0, sizeof(*p));
  p->enabled = false;
  p->channels = DSP_CHANNELS_STEREO;
}

bool
dsp_profile_is_neutral(const struct dsp_profile *p)
{
  int i;

  if (!p->enabled)
    return true;
  if (p->preamp_db != 0.0f || p->highpass_hz > 0 || p->lowpass_hz > 0 || p->channels != DSP_CHANNELS_STEREO)
    return false;
  for (i = 0; i < DSP_BANDS; i++)
    if (p->band_db[i] != 0.0f)
      return false;

  return true;
}

bool
dsp_profile_equal(const struct dsp_profile *a, const struct dsp_profile *b)
{
  int i;

  // All neutral profiles are the same, whatever their values
  if (dsp_profile_is_neutral(a) && dsp_profile_is_neutral(b))
    return true;
  if (a->enabled != b->enabled || a->preamp_db != b->preamp_db || a->highpass_hz != b->highpass_hz ||
      a->lowpass_hz != b->lowpass_hz || a->channels != b->channels)
    return false;
  for (i = 0; i < DSP_BANDS; i++)
    if (a->band_db[i] != b->band_db[i])
      return false;

  return true;
}

const char *
dsp_channels_to_string(enum dsp_channels channels)
{
  if ((int)channels < 0 || (size_t)channels >= ARRAY_SIZE(dsp_channels_str))
    return "stereo";
  return dsp_channels_str[channels];
}

static float
clampf(double v, double lo, double hi)
{
  if (isnan(v))
    return 0.0f;
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  // Store with 0.1 dB resolution so equal looking profiles compare equal
  return roundf((float)v * 10.0f) / 10.0f;
}

static int
clamp_hz(int v)
{
  if (v <= 0)
    return 0;
  if (v < 20)
    return 20;
  if (v > 20000)
    return 20000;
  return v;
}

int
dsp_profile_from_json(struct dsp_profile *p, const char *json)
{
  struct dsp_profile out;
  json_object *root;
  json_object *item;
  json_object *band;
  json_object *gain;
  const char *str;
  size_t n;
  size_t i;
  int j;

  root = json_tokener_parse(json);
  if (!root || !json_object_is_type(root, json_type_object))
    {
      json_object_put(root);
      return -1;
    }

  dsp_profile_default(&out);

  if (json_object_object_get_ex(root, "enabled", &item))
    out.enabled = json_object_get_boolean(item);
  if (json_object_object_get_ex(root, "preamp_db", &item))
    out.preamp_db = clampf(json_object_get_double(item), -24.0, 12.0);
  if (json_object_object_get_ex(root, "highpass_hz", &item))
    out.highpass_hz = clamp_hz(json_object_get_int(item));
  if (json_object_object_get_ex(root, "lowpass_hz", &item))
    out.lowpass_hz = clamp_hz(json_object_get_int(item));
  if (json_object_object_get_ex(root, "channels", &item) && (str = json_object_get_string(item)))
    {
      for (j = 0; j < (int)ARRAY_SIZE(dsp_channels_str); j++)
	if (strcmp(str, dsp_channels_str[j]) == 0)
	  out.channels = j;
    }

  // "bands" is an array of {"freq": 60, "gain_db": 3.0} (freq is informational,
  // the position decides the band) or of plain numbers
  if (json_object_object_get_ex(root, "bands", &item) && json_object_is_type(item, json_type_array))
    {
      n = json_object_array_length(item);
      for (i = 0; i < n && i < DSP_BANDS; i++)
	{
	  band = json_object_array_get_idx(item, i);
	  if (json_object_is_type(band, json_type_object) && json_object_object_get_ex(band, "gain_db", &gain))
	    out.band_db[i] = clampf(json_object_get_double(gain), -12.0, 12.0);
	  else if (json_object_is_type(band, json_type_double) || json_object_is_type(band, json_type_int))
	    out.band_db[i] = clampf(json_object_get_double(band), -12.0, 12.0);
	}
    }

  json_object_put(root);

  // A crossover where the high pass is above the low pass would mute the
  // speaker, treat that as a mistake
  if (out.highpass_hz && out.lowpass_hz && out.highpass_hz >= out.lowpass_hz)
    return -1;

  *p = out;
  return 0;
}

char *
dsp_profile_to_json(const struct dsp_profile *p)
{
  json_object *root = json_object_new_object();
  json_object *bands = json_object_new_array();
  json_object *band;
  char *out;
  int i;

  json_object_object_add(root, "enabled", json_object_new_boolean(p->enabled));
  json_object_object_add(root, "preamp_db", json_object_new_double(p->preamp_db));
  for (i = 0; i < DSP_BANDS; i++)
    {
      band = json_object_new_object();
      json_object_object_add(band, "freq", json_object_new_int(dsp_band_freq[i]));
      json_object_object_add(band, "type", json_object_new_string(i == 0 ? "lowshelf" : (i == DSP_BANDS - 1) ? "highshelf" : "peaking"));
      json_object_object_add(band, "gain_db", json_object_new_double(p->band_db[i]));
      json_object_array_add(bands, band);
    }
  json_object_object_add(root, "bands", bands);
  json_object_object_add(root, "highpass_hz", json_object_new_int(p->highpass_hz));
  json_object_object_add(root, "lowpass_hz", json_object_new_int(p->lowpass_hz));
  json_object_object_add(root, "channels", json_object_new_string(dsp_channels_to_string(p->channels)));

  out = strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
  json_object_put(root);
  return out;
}


/* --------------------------- Filter coefficients --------------------------- */

static void
biquad_set(struct biquad *bq, double b0, double b1, double b2, double a0, double a1, double a2)
{
  bq->b0 = b0 / a0;
  bq->b1 = b1 / a0;
  bq->b2 = b2 / a0;
  bq->a1 = a1 / a0;
  bq->a2 = a2 / a0;
}

static void
biquad_peaking(struct biquad *bq, double fs, double f, double gain_db, double q)
{
  double A = pow(10.0, gain_db / 40.0);
  double w0 = 2.0 * M_PI * f / fs;
  double alpha = sin(w0) / (2.0 * q);
  double c = cos(w0);

  biquad_set(bq, 1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
}

// Shelf slope S = 1 (steepest without overshoot)
static void
biquad_shelf(struct biquad *bq, double fs, double f, double gain_db, bool high)
{
  double A = pow(10.0, gain_db / 40.0);
  double w0 = 2.0 * M_PI * f / fs;
  double c = cos(w0);
  double alpha = sin(w0) / 2.0 * sqrt(2.0);
  double sa = 2.0 * sqrt(A) * alpha;

  if (!high)
    biquad_set(bq,
	       A * ((A + 1) - (A - 1) * c + sa),
	       2 * A * ((A - 1) - (A + 1) * c),
	       A * ((A + 1) - (A - 1) * c - sa),
	       (A + 1) + (A - 1) * c + sa,
	       -2 * ((A - 1) + (A + 1) * c),
	       (A + 1) + (A - 1) * c - sa);
  else
    biquad_set(bq,
	       A * ((A + 1) + (A - 1) * c + sa),
	       -2 * A * ((A - 1) + (A + 1) * c),
	       A * ((A + 1) + (A - 1) * c - sa),
	       (A + 1) - (A - 1) * c + sa,
	       2 * ((A - 1) - (A + 1) * c),
	       (A + 1) - (A - 1) * c - sa);
}

static void
biquad_pass(struct biquad *bq, double fs, double f, bool high)
{
  double w0 = 2.0 * M_PI * f / fs;
  double c = cos(w0);
  double alpha = sin(w0) / (2.0 * DSP_BUTTERWORTH_Q);

  if (high)
    biquad_set(bq, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
  else
    biquad_set(bq, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

static void
coefficients_compute(struct dsp_ctx *ctx)
{
  const struct dsp_profile *p = &ctx->profile;
  double fs = ctx->sample_rate;
  double nyq = fs / 2.0 * 0.95;
  int n = 0;
  int i;

  ctx->preamp = pow(10.0, p->preamp_db / 20.0);

  for (i = 0; i < DSP_BANDS; i++)
    {
      if (p->band_db[i] == 0.0f || dsp_band_freq[i] >= nyq)
	continue;

      if (i == 0)
	biquad_shelf(&ctx->sections[n++], fs, dsp_band_freq[i], p->band_db[i], false);
      else if (i == DSP_BANDS - 1)
	biquad_shelf(&ctx->sections[n++], fs, dsp_band_freq[i], p->band_db[i], true);
      else
	biquad_peaking(&ctx->sections[n++], fs, dsp_band_freq[i], p->band_db[i], DSP_PEAK_Q);
    }

  // Linkwitz-Riley 4th order = two identical Butterworth sections
  if (p->highpass_hz > 0 && p->highpass_hz < nyq)
    {
      biquad_pass(&ctx->sections[n++], fs, p->highpass_hz, true);
      biquad_pass(&ctx->sections[n++], fs, p->highpass_hz, true);
    }
  if (p->lowpass_hz > 0 && p->lowpass_hz < nyq)
    {
      biquad_pass(&ctx->sections[n++], fs, p->lowpass_hz, false);
      biquad_pass(&ctx->sections[n++], fs, p->lowpass_hz, false);
    }

  // If the number of sections changed, the old state belongs to other filters
  if (n != ctx->nsections)
    {
      memset(ctx->z1, 0, sizeof(ctx->z1));
      memset(ctx->z2, 0, sizeof(ctx->z2));
    }
  ctx->nsections = n;
}


/* -------------------------------- Processing ------------------------------- */

struct dsp_ctx *
dsp_new(const struct dsp_profile *p, int sample_rate)
{
  struct dsp_ctx *ctx;

  CHECK_NULL(L_PLAYER, ctx = calloc(1, sizeof(struct dsp_ctx)));
  ctx->profile = *p;
  ctx->sample_rate = (sample_rate > 0) ? sample_rate : 44100;
  coefficients_compute(ctx);
  return ctx;
}

void
dsp_update(struct dsp_ctx *ctx, const struct dsp_profile *p)
{
  if (!ctx)
    return;

  ctx->profile = *p;
  coefficients_compute(ctx);
}

void
dsp_free(struct dsp_ctx *ctx)
{
  free(ctx);
}

uint64_t
dsp_clips_take(struct dsp_ctx *ctx)
{
  uint64_t clips;

  if (!ctx)
    return 0;

  clips = ctx->clips;
  ctx->clips = 0;
  return clips;
}

static inline double
section_run(struct dsp_ctx *ctx, int ch, int s, double x)
{
  const struct biquad *bq = &ctx->sections[s];
  double y = bq->b0 * x + ctx->z1[ch][s];

  ctx->z1[ch][s] = bq->b1 * x - bq->a1 * y + ctx->z2[ch][s];
  ctx->z2[ch][s] = bq->b2 * x - bq->a2 * y;
  return y;
}

static inline int16_t
to_s16(struct dsp_ctx *ctx, double v)
{
  if (v > 32767.0)
    {
      ctx->clips++;
      return INT16_MAX;
    }
  if (v < -32768.0)
    {
      ctx->clips++;
      return INT16_MIN;
    }
  return (int16_t)lrint(v);
}

void
dsp_process_s16(struct dsp_ctx *ctx, int16_t *samples, size_t frames, int channels)
{
  double in[DSP_MAX_CHANNELS];
  double v;
  size_t f;
  int ch;
  int s;

  if (!ctx || !ctx->profile.enabled || channels < 1 || channels > DSP_MAX_CHANNELS)
    return;

  for (f = 0; f < frames; f++)
    {
      int16_t *frame = samples + f * channels;

      for (ch = 0; ch < channels; ch++)
	in[ch] = frame[ch];

      if (channels == 2)
	{
	  switch (ctx->profile.channels)
	    {
	      case DSP_CHANNELS_LEFT:
		in[1] = in[0];
		break;
	      case DSP_CHANNELS_RIGHT:
		in[0] = in[1];
		break;
	      case DSP_CHANNELS_MONO:
		in[0] = in[1] = (in[0] + in[1]) / 2.0;
		break;
	      case DSP_CHANNELS_SWAP:
		v = in[0];
		in[0] = in[1];
		in[1] = v;
		break;
	      default:
		break;
	    }
	}

      for (ch = 0; ch < channels; ch++)
	{
	  v = in[ch] * ctx->preamp;
	  for (s = 0; s < ctx->nsections; s++)
	    v = section_run(ctx, ch, s, v);
	  frame[ch] = to_s16(ctx, v);
	}
    }
}


/* --------------------------------- Storage --------------------------------- */

// Caller must hold dsp_cache_lck
static struct dsp_cache_entry *
cache_find(uint64_t device_id)
{
  int i;

  for (i = 0; i < DSP_CACHE_SIZE; i++)
    if (dsp_cache[i].in_use && dsp_cache[i].device_id == device_id)
      return &dsp_cache[i];

  return NULL;
}

// Caller must hold dsp_cache_lck
static struct dsp_cache_entry *
cache_add(uint64_t device_id, const struct dsp_profile *p)
{
  struct dsp_cache_entry *e = NULL;
  int i;

  for (i = 0; i < DSP_CACHE_SIZE && !e; i++)
    if (!dsp_cache[i].in_use)
      e = &dsp_cache[i];

  // Full: just reuse the first one, the database still has it
  if (!e)
    e = &dsp_cache[0];

  e->in_use = true;
  e->device_id = device_id;
  e->profile = *p;
  return e;
}

void
dsp_profile_get(struct dsp_profile *p, uint64_t device_id)
{
  struct dsp_cache_entry *e;
  char key[64];
  char *json = NULL;

  pthread_mutex_lock(&dsp_cache_lck);
  e = cache_find(device_id);
  if (e)
    {
      *p = e->profile;
      pthread_mutex_unlock(&dsp_cache_lck);
      return;
    }
  pthread_mutex_unlock(&dsp_cache_lck);

  dsp_profile_default(p);

  snprintf(key, sizeof(key), DSP_ADMIN_KEY_FMT, device_id);
  if (db_admin_get(&json, key) == 0 && json)
    {
      if (dsp_profile_from_json(p, json) < 0)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Ignoring invalid stored EQ profile for device %" PRIu64 "\n", device_id);
	  dsp_profile_default(p);
	}
    }
  free(json);

  pthread_mutex_lock(&dsp_cache_lck);
  if (!cache_find(device_id))
    cache_add(device_id, p);
  pthread_mutex_unlock(&dsp_cache_lck);
}

int
dsp_profile_set(const struct dsp_profile *p, uint64_t device_id)
{
  struct dsp_cache_entry *e;
  char key[64];
  char *json;
  int ret;

  json = dsp_profile_to_json(p);
  snprintf(key, sizeof(key), DSP_ADMIN_KEY_FMT, device_id);
  ret = db_admin_set(key, json);
  free(json);
  if (ret < 0)
    return -1;

  pthread_mutex_lock(&dsp_cache_lck);
  e = cache_find(device_id);
  if (e)
    e->profile = *p;
  else
    cache_add(device_id, p);
  __atomic_add_fetch(&dsp_generation, 1, __ATOMIC_SEQ_CST);
  pthread_mutex_unlock(&dsp_cache_lck);

  return 0;
}

uint32_t
dsp_generation_get(void)
{
  return __atomic_load_n(&dsp_generation, __ATOMIC_SEQ_CST);
}
