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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <json.h>

#include "speaker_test.h"
#include "airplay_receiver.h"
#include "db.h"
#include "logger.h"
#include "player.h"
#include "stats.h"

#define TEST_PATH            "/tmp/owntone-speaker-test.wav"
#define TEST_RATE            44100
#define TEST_TONE_MS         1000
#define TEST_TONE_HZ         1000
#define TEST_LEVEL_DBFS      -12.0
// Silence after the tone, so the test can stop the player before the end and
// it never moves on to the next item in the queue
#define TEST_TAIL_MS         500
#define TEST_FADE_MS         10
#define TEST_VOLUME          50
#define TEST_START_TIMEOUT_S 15
#define TEST_MAX_SPEAKERS    64

struct speaker_test
{
  pthread_mutex_t lck;
  bool running;
  uint64_t speaker_id;
  char name[255];
  char output_type[50];
  int volume; // The speaker's own volume, put back after the test
  const char *state; // idle, running, passed, failed
  char message[320];
  uint64_t packets;
};

static struct speaker_test test = { .lck = PTHREAD_MUTEX_INITIALIZER, .state = "idle" };

struct selected_speakers
{
  uint64_t ids[TEST_MAX_SPEAKERS + 1]; // ids[0] is the count, as player_speaker_set() wants
  int volumes[TEST_MAX_SPEAKERS + 1];  // Their volumes, same index
};


/* ------------------------------ Test sound -------------------------------- */

static inline void
le16_put(uint8_t *p, uint16_t v)
{
  p[0] = v & 0xff;
  p[1] = v >> 8;
}

static inline void
le32_put(uint8_t *p, uint32_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = v >> 24;
}

// A 1 kHz sine tone with short fades, then silence. Both channels are the
// same so every channel routing plays it.
static int
sound_write(const char *path)
{
  int tone = TEST_RATE * TEST_TONE_MS / 1000;
  int total = tone + TEST_RATE * TEST_TAIL_MS / 1000;
  int fade = TEST_RATE * TEST_FADE_MS / 1000;
  uint32_t data_len = total * 2 * sizeof(int16_t);
  float amp = powf(10.0f, TEST_LEVEL_DBFS / 20.0f);
  uint8_t hdr[44];
  int16_t *pcm;
  float v;
  FILE *f;
  size_t n;
  int i;

  CHECK_NULL(L_PLAYER, pcm = calloc(total * 2, sizeof(int16_t)));

  for (i = 0; i < tone; i++)
    {
      v = amp * sinf(2.0f * (float)M_PI * TEST_TONE_HZ * i / TEST_RATE);
      if (i < fade)
	v *= (float)i / fade;
      else if (i >= tone - fade)
	v *= (float)(tone - i) / fade;
      pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(v * 32767.0f);
    }

  memcpy(hdr, "RIFF", 4);
  le32_put(hdr + 4, 36 + data_len);
  memcpy(hdr + 8, "WAVEfmt ", 8);
  le32_put(hdr + 16, 16);
  le16_put(hdr + 20, 1);   // PCM
  le16_put(hdr + 22, 2);   // Channels
  le32_put(hdr + 24, TEST_RATE);
  le32_put(hdr + 28, TEST_RATE * 2 * sizeof(int16_t));
  le16_put(hdr + 32, 2 * sizeof(int16_t));
  le16_put(hdr + 34, 16);
  memcpy(hdr + 36, "data", 4);
  le32_put(hdr + 40, data_len);

  for (i = 0; i < total * 2; i++)
    le16_put((uint8_t *)&pcm[i], (uint16_t)pcm[i]);

  f = fopen(path, "wb");
  if (!f)
    {
      DPRINTF(E_LOG, L_PLAYER, "Speaker test: could not write '%s'\n", path);
      free(pcm);
      return -1;
    }

  n = fwrite(hdr, 1, sizeof(hdr), f);
  n += fwrite(pcm, 1, data_len, f);
  fclose(f);
  free(pcm);

  return (n == sizeof(hdr) + data_len) ? 0 : -1;
}


/* -------------------------------- The test -------------------------------- */

static void
selected_cb(struct player_speaker_info *spk, void *arg)
{
  struct selected_speakers *sel = arg;

  if (spk->selected && sel->ids[0] < TEST_MAX_SPEAKERS)
    {
      sel->ids[++sel->ids[0]] = spk->id;
      sel->volumes[sel->ids[0]] = spk->absvol;
    }
}

static void
finish(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
finish(const char *state, const char *fmt, ...)
{
  va_list ap;

  pthread_mutex_lock(&test.lck);
  test.state = state;
  va_start(ap, fmt);
  vsnprintf(test.message, sizeof(test.message), fmt, ap);
  va_end(ap);
  test.running = false;
  DPRINTF(E_LOG, L_PLAYER, "Speaker test of '%s' %s: %s\n", test.name, state, test.message);
  pthread_mutex_unlock(&test.lck);
}

static void *
test_thread(void *arg)
{
  struct selected_speakers saved = { { 0 }, { 0 } };
  struct player_status status;
  struct db_queue_add_info add_info;
  struct db_queue_item qi;
  struct db_queue_item *item;
  uint32_t item_id = 0;
  uint64_t before[STATS_OUTPUT_COUNTERS];
  uint64_t after[STATS_OUTPUT_COUNTERS];
  uint64_t only[2];
  uint64_t packets;
  uint64_t failures;
  bool started = false;
  bool airplay;
  char name[255];
  int volume;
  int sound_ms;
  int ret;
  int i;

  pthread_mutex_lock(&test.lck);
  only[0] = 1;
  only[1] = test.speaker_id;
  snprintf(name, sizeof(name), "%s", test.name);
  airplay = (strncmp(test.output_type, "AirPlay", 7) == 0);
  volume = test.volume;
  pthread_mutex_unlock(&test.lck);

  sound_ms = TEST_TONE_MS;
  if (sound_write(TEST_PATH) < 0)
    {
      finish("failed", "Could not create the test tone");
      return NULL;
    }

  if (db_perthread_init() < 0)
    {
      finish("failed", "Could not open the database");
      return NULL;
    }

  player_speaker_enumerate(selected_cb, &saved);
  stats_output_counters_get(only[1], before);

  ret = player_speaker_set(only);
  if (ret < 0)
    {
      finish("failed", "Could not select %s on its own", name);
      goto restore;
    }

  player_volume_setabs_speaker(only[1], TEST_VOLUME);

  // The queue code fixes up (frees and replaces) these, so they must be ours
  memset(&qi, 0, sizeof(qi));
  qi.path = strdup(TEST_PATH);
  qi.virtual_path = strdup("/" TEST_PATH);
  qi.title = strdup("Speaker test");
  qi.artist = strdup("OwnTone");
  qi.album = strdup("Speaker test");
  qi.type = strdup("wav");
  qi.data_kind = DATA_KIND_FILE;
  qi.media_kind = MEDIA_KIND_MUSIC;
  qi.song_length = sound_ms + TEST_TAIL_MS;

  // The input only plays queue items, so the sound goes at the end of the
  // queue for the test and is removed again afterwards
  ret = db_queue_add_start(&add_info, -1);
  if (ret == 0)
    {
      ret = db_queue_add_next(&add_info, &qi);
      ret = db_queue_add_end(&add_info, 0, 0, ret);
    }
  free_queue_item(&qi, 1);
  item = (ret < 0) ? NULL : db_queue_fetch_byitemid(add_info.new_item_id);
  if (!item)
    {
      finish("failed", "Could not queue the test tone");
      goto restore;
    }

  item_id = item->id;
  ret = player_playback_start_byitem(item);
  free_queue_item(item, 0);
  if (ret < 0)
    {
      finish("failed", "Playback could not start on %s", name);
      goto restore;
    }

  // Wait for the noise to play out. If the player ever reports another item,
  // it moved on to the queue, so stop.
  for (i = 0; i < TEST_START_TIMEOUT_S * 10; i++)
    {
      usleep(100000);
      if (player_get_status(&status) < 0)
	break;

      if (status.status == PLAY_PLAYING)
	started = true;
      if (started && (status.status != PLAY_PLAYING || status.item_id != item_id || status.pos_ms >= (uint32_t)sound_ms + 200))
	break;
    }

  if (player_get_status(&status) == 0 && status.status != PLAY_STOPPED)
    player_playback_stop();

  stats_output_counters_get(only[1], after);
  packets = after[STATS_OUTPUT_PACKETS] - before[STATS_OUTPUT_PACKETS];
  failures = after[STATS_OUTPUT_FAILURES] - before[STATS_OUTPUT_FAILURES];

  pthread_mutex_lock(&test.lck);
  test.packets = packets;
  pthread_mutex_unlock(&test.lck);

  if (!started)
    finish("failed", "%s did not start playing within %d s", name, TEST_START_TIMEOUT_S);
  else if (failures && !packets)
    finish("failed", "%s did not accept the audio session", name);
  else if (failures)
    finish("failed", "%s dropped out during the test (%" PRIu64 " packets sent)", name, packets);
  else if (packets)
    finish("passed", "%s connected and received %" PRIu64 " audio packets without errors", name, packets);
  else if (airplay)
    finish("failed", "%s connected but no audio was sent to it", name);
  else
    finish("passed", "Played on %s (this kind of speaker doesn't report delivery)", name);

 restore:
  if (item_id)
    db_queue_delete_byitemid(item_id);
  // Selecting speakers rescales their volumes, so all of them are put back
  if (saved.ids[0] > 0 && player_speaker_set(saved.ids) < 0)
    DPRINTF(E_LOG, L_PLAYER, "Speaker test: could not restore the speaker selection\n");
  for (i = 1; i <= (int)saved.ids[0]; i++)
    player_volume_setabs_speaker(saved.ids[i], saved.volumes[i]);
  player_volume_setabs_speaker(only[1], volume);

  db_perthread_deinit();
  return NULL;
}

// A paused AirPlay stream keeps the player "playing" while it waits for more
// audio, but nothing is heard, so it counts as idle
static bool
now_playing_is_airplay(uint32_t item_id)
{
  struct db_queue_item *item;
  bool airplay;

  item = db_queue_fetch_byitemid(item_id);
  if (!item)
    return false;

  airplay = (item->data_kind == DATA_KIND_AIRPLAY);
  free_queue_item(item, 0);
  return airplay;
}

enum speaker_test_result
speaker_test_start(uint64_t speaker_id)
{
  struct player_speaker_info spk;
  struct player_status status;
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  if (player_speaker_get_byid(&spk, speaker_id) < 0)
    return SPEAKER_TEST_ERR_UNKNOWN_SPEAKER;

  if (player_get_status(&status) < 0)
    return SPEAKER_TEST_ERR_INTERNAL;
  if (status.status == PLAY_PLAYING && !(now_playing_is_airplay(status.item_id) && airplay_receiver_is_idle()))
    return SPEAKER_TEST_ERR_NOT_IDLE;

  pthread_mutex_lock(&test.lck);
  if (test.running)
    {
      pthread_mutex_unlock(&test.lck);
      return SPEAKER_TEST_ERR_BUSY;
    }

  test.running = true;
  test.speaker_id = speaker_id;
  snprintf(test.name, sizeof(test.name), "%s", spk.name);
  snprintf(test.output_type, sizeof(test.output_type), "%s", spk.output_type);
  test.volume = spk.absvol;
  test.state = "running";
  snprintf(test.message, sizeof(test.message), "Playing the test tone on %s", spk.name);
  test.packets = 0;
  pthread_mutex_unlock(&test.lck);

  DPRINTF(E_LOG, L_PLAYER, "Speaker test of '%s' starting\n", spk.name);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&tid, &attr, test_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      finish("failed", "Could not start the test");
      return SPEAKER_TEST_ERR_INTERNAL;
    }

  return SPEAKER_TEST_STARTED;
}

void
speaker_test_json_add(json_object *obj)
{
  char id[24];

  pthread_mutex_lock(&test.lck);
  snprintf(id, sizeof(id), "%" PRIu64, test.speaker_id);
  json_object_object_add(obj, "speaker_id", json_object_new_string(test.speaker_id ? id : ""));
  json_object_object_add(obj, "speaker_name", json_object_new_string(test.name));
  json_object_object_add(obj, "state", json_object_new_string(test.state));
  json_object_object_add(obj, "message", json_object_new_string(test.message));
  json_object_object_add(obj, "packets", json_object_new_int64(test.packets));
  pthread_mutex_unlock(&test.lck);
}
