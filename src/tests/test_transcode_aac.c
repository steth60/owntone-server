/*
 * Test for the XCODE_AAC profile and the per-packet encoder API
 * (transcode_encode_raw / transcode_encode_packet_get), which the AirPlay 2
 * buffered audio stream relies on.
 *
 * Feeds a sine wave to the encoder in 352-sample chunks (the way airplay.c
 * feeds its encoder today), checks that complete AAC-LC access units of 1024
 * samples come out one at a time, and writes them as an ADTS file so the
 * wrapper script can verify them with ffprobe/ffmpeg.
 *
 * Usage: test_transcode_aac <output.adts> <owntone.conf>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <event2/buffer.h>

#include "logger.h"
#include "conffile.h"
#include "misc.h"
#include "transcode.h"

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BPS           16
#define SECONDS       2
#define CHUNK_SAMPLES 352   // What airplay.c pushes per encode call
#define AAC_FRAME     1024  // Samples per AAC-LC access unit
#define MAX_PACKET    4096  // Shairport Sync suggests this is the largest buffered packet seen

#define TOTAL_SAMPLES (SAMPLE_RATE * SECONDS)
#define MIN_PACKETS   (TOTAL_SAMPLES / AAC_FRAME)       // 86
#define MAX_PACKETS   (MIN_PACKETS + 3)                 // flush + encoder priming

static int failures;

#define CHECK(cond, ...) \
  do { if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void
adts_header_make(uint8_t h[7], int aac_len)
{
  int len = aac_len + 7;

  h[0] = 0xFF;
  h[1] = 0xF1;                                   // MPEG-4, layer 0, no CRC
  h[2] = (1 << 6) | (4 << 2) | (CHANNELS >> 2);  // profile LC (2-1), sfi 4 = 44100 Hz
  h[3] = ((CHANNELS & 3) << 6) | ((len >> 11) & 3);
  h[4] = (len >> 3) & 0xFF;
  h[5] = ((len & 7) << 5) | 0x1F;
  h[6] = 0xFC;
}

static void
sine_fill(int16_t *pcm, int nsamples, int *phase)
{
  for (int i = 0; i < nsamples; i++, (*phase)++)
    {
      int16_t v = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * (*phase) / SAMPLE_RATE));
      pcm[2 * i] = v;
      pcm[2 * i + 1] = v;
    }
}

// Pops every complete packet from the encoder, validates it and writes it as ADTS
static int
packets_drain(FILE *out, struct encode_ctx *ctx, struct evbuffer *evbuf, int *max_size)
{
  uint8_t adts[7];
  int npackets = 0;
  int size;

  while ((size = transcode_encode_packet_get(evbuf, ctx)) != 0)
    {
      CHECK(size > 0, "packet get returned %d", size);
      if (size < 0)
	return -1;

      CHECK(size <= MAX_PACKET, "packet of %d bytes exceeds %d", size, MAX_PACKET);
      CHECK((int)evbuffer_get_length(evbuf) == size, "evbuf holds %zu bytes but packet is %d", evbuffer_get_length(evbuf), size);

      if (size > *max_size)
	*max_size = size;

      adts_header_make(adts, size);
      fwrite(adts, 1, sizeof(adts), out);
      fwrite(evbuffer_pullup(evbuf, size), 1, size, out);
      evbuffer_drain(evbuf, size);
      npackets++;
    }

  return npackets;
}

int
main(int argc, char **argv)
{
  struct media_quality quality = { .sample_rate = SAMPLE_RATE, .bits_per_sample = BPS, .channels = CHANNELS };
  struct transcode_encode_setup_args encode_args = { .profile = XCODE_AAC, .quality = &quality };
  struct decode_ctx *src_ctx;
  struct encode_ctx *encode_ctx;
  struct evbuffer *evbuf;
  transcode_frame *frame;
  int16_t pcm[CHUNK_SAMPLES * CHANNELS];
  FILE *out;
  int phase = 0;
  int fed = 0;
  int packets_before_flush = 0;
  int packets_total = 0;
  int max_size = 0;
  int ret;

  if (argc < 3)
    {
      fprintf(stderr, "Usage: %s <output.adts> <owntone.conf>\n", argv[0]);
      return 2;
    }

  ret = logger_init(NULL, NULL, E_LOG, NULL);
  CHECK(ret == 0, "logger_init failed");

  ret = conffile_load(argv[2]);
  CHECK(ret == 0, "could not load config '%s'", argv[2]);
  if (ret < 0)
    return 1;

  out = fopen(argv[1], "wb");
  CHECK(out != NULL, "could not open '%s' for writing", argv[1]);
  if (!out)
    return 1;

  // Same setup sequence as airplay.c's master_session_make()
  src_ctx = transcode_decode_setup_raw(XCODE_PCM16, &quality);
  CHECK(src_ctx != NULL, "transcode_decode_setup_raw failed");
  if (!src_ctx)
    return 1;

  encode_args.src_ctx = src_ctx;
  encode_ctx = transcode_encode_setup(encode_args);
  transcode_decode_cleanup(&src_ctx);
  CHECK(encode_ctx != NULL, "transcode_encode_setup(XCODE_AAC) failed - ffmpeg without an AAC encoder?");
  if (!encode_ctx)
    return 1;

  ret = transcode_encode_query(encode_ctx, "samples_per_frame");
  CHECK(ret == AAC_FRAME, "encoder frame size is %d, expected %d", ret, AAC_FRAME);
  ret = transcode_encode_query(encode_ctx, "sample_rate");
  CHECK(ret == SAMPLE_RATE, "encoder sample rate is %d, expected %d", ret, SAMPLE_RATE);
  ret = transcode_encode_query(encode_ctx, "channels");
  CHECK(ret == CHANNELS, "encoder channels is %d, expected %d", ret, CHANNELS);

  // The raw API must refuse a non-raw profile
  encode_args.profile = XCODE_ALAC;
  src_ctx = transcode_decode_setup_raw(XCODE_PCM16, &quality);
  encode_args.src_ctx = src_ctx;
  {
    struct encode_ctx *alac_ctx = transcode_encode_setup(encode_args);
    transcode_decode_cleanup(&src_ctx);
    if (alac_ctx)
      {
	ret = transcode_encode_raw(alac_ctx, NULL, 1);
	CHECK(ret < 0, "transcode_encode_raw accepted the ALAC profile");
	transcode_encode_cleanup(&alac_ctx);
      }
  }

  evbuf = evbuffer_new();

  while (fed < TOTAL_SAMPLES)
    {
      sine_fill(pcm, CHUNK_SAMPLES, &phase);
      fed += CHUNK_SAMPLES;

      frame = transcode_frame_new(pcm, sizeof(pcm), CHUNK_SAMPLES, &quality);
      CHECK(frame != NULL, "transcode_frame_new failed");

      ret = transcode_encode_raw(encode_ctx, frame, 0);
      transcode_frame_free(frame);
      CHECK(ret >= 0, "transcode_encode_raw failed (%d) after %d samples", ret, fed);
      if (ret < 0)
	break;

      ret = packets_drain(out, encode_ctx, evbuf, &max_size);
      if (ret < 0)
	break;

      packets_before_flush += ret;
    }

  // Flush
  ret = transcode_encode_raw(encode_ctx, NULL, 1);
  CHECK(ret >= 0, "flush failed (%d)", ret);
  packets_total = packets_before_flush + packets_drain(out, encode_ctx, evbuf, &max_size);

  // Nothing may be left behind
  ret = transcode_encode_packet_get(evbuf, encode_ctx);
  CHECK(ret == 0, "encoder still had a packet after draining (%d)", ret);

  CHECK(packets_before_flush >= MIN_PACKETS - 2, "only %d packets came out before the flush, encoder is hoarding", packets_before_flush);
  CHECK(packets_total >= MIN_PACKETS && packets_total <= MAX_PACKETS, "%d packets for %d samples, expected %d-%d", packets_total, TOTAL_SAMPLES, MIN_PACKETS, MAX_PACKETS);

  printf("aac: %d samples in, %d packets out (%d before flush), largest packet %d bytes\n", fed, packets_total, packets_before_flush, max_size);

  evbuffer_free(evbuf);
  transcode_encode_cleanup(&encode_ctx);
  fclose(out);
  conffile_unload();
  logger_deinit();

  if (failures)
    {
      fprintf(stderr, "%d check(s) failed\n", failures);
      return 1;
    }

  printf("ok\n");
  return 0;
}
