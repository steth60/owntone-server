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
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <json.h>

#include "stats.h"

#define STATS_MAX_OUTPUTS 64

struct stats_output
{
  bool in_use;
  uint64_t device_id;
  uint64_t counters[STATS_OUTPUT_COUNTERS];
  char last_event[96];
  time_t last_event_time;
  char transport[24];
  bool eq_active;
  int encoder_sessions;
};

static const char *stats_output_names[STATS_OUTPUT_COUNTERS] =
{
  "packets", "bytes", "send_errors", "retransmit_requests", "retransmits",
  "retransmits_missed", "sessions", "failures", "eq_clips",
};

static const char *stats_player_names[STATS_PLAYER_COUNTERS] =
{
  "starts", "underruns", "flushes", "read_errors",
};

static pthread_mutex_t stats_lck = PTHREAD_MUTEX_INITIALIZER;
static struct stats_output stats_outputs[STATS_MAX_OUTPUTS];
static uint64_t stats_player[STATS_PLAYER_COUNTERS];


// Caller must hold stats_lck
static struct stats_output *
output_get(uint64_t device_id)
{
  struct stats_output *free_slot = NULL;
  int i;

  for (i = 0; i < STATS_MAX_OUTPUTS; i++)
    {
      if (stats_outputs[i].in_use && stats_outputs[i].device_id == device_id)
	return &stats_outputs[i];
      if (!stats_outputs[i].in_use && !free_slot)
	free_slot = &stats_outputs[i];
    }

  if (!free_slot)
    return NULL;

  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->in_use = true;
  free_slot->device_id = device_id;
  return free_slot;
}

void
stats_output_add(uint64_t device_id, enum stats_output_counter counter, uint64_t n)
{
  struct stats_output *o;

  if (counter >= STATS_OUTPUT_COUNTERS)
    return;

  pthread_mutex_lock(&stats_lck);
  o = output_get(device_id);
  if (o)
    o->counters[counter] += n;
  pthread_mutex_unlock(&stats_lck);
}

void
stats_output_counters_get(uint64_t device_id, uint64_t counters[STATS_OUTPUT_COUNTERS])
{
  struct stats_output *o;

  pthread_mutex_lock(&stats_lck);
  o = output_get(device_id);
  if (o)
    memcpy(counters, o->counters, sizeof(o->counters));
  else
    memset(counters, 0, STATS_OUTPUT_COUNTERS * sizeof(uint64_t));
  pthread_mutex_unlock(&stats_lck);
}

void
stats_output_event(uint64_t device_id, const char *event)
{
  struct stats_output *o;

  pthread_mutex_lock(&stats_lck);
  o = output_get(device_id);
  if (o)
    {
      snprintf(o->last_event, sizeof(o->last_event), "%s", event);
      o->last_event_time = time(NULL);
    }
  pthread_mutex_unlock(&stats_lck);
}

void
stats_output_session_set(uint64_t device_id, const char *transport, bool eq_active, int encoder_sessions)
{
  struct stats_output *o;

  pthread_mutex_lock(&stats_lck);
  o = output_get(device_id);
  if (o)
    {
      snprintf(o->transport, sizeof(o->transport), "%s", transport ? transport : "");
      o->eq_active = eq_active;
      o->encoder_sessions = encoder_sessions;
    }
  pthread_mutex_unlock(&stats_lck);
}

void
stats_player_add(enum stats_player_counter counter, uint64_t n)
{
  if (counter >= STATS_PLAYER_COUNTERS)
    return;

  pthread_mutex_lock(&stats_lck);
  stats_player[counter] += n;
  pthread_mutex_unlock(&stats_lck);
}


/* ------------------------------ Host figures ------------------------------- */

// Set when the program is loaded. Computing uptime from /proc doesn't work in
// containers, where /proc/uptime is the container's but the process start
// time counts from the host's boot.
static time_t stats_process_start;

__attribute__((constructor)) static void
stats_process_start_set(void)
{
  stats_process_start = time(NULL);
}

static void
process_json_add(json_object *obj)
{
  json_object *proc = json_object_new_object();
  long ticks = sysconf(_SC_CLK_TCK);
  long page = sysconf(_SC_PAGESIZE);
  unsigned long utime = 0;
  unsigned long stime = 0;
  unsigned long vsize_pages = 0;
  unsigned long rss_pages = 0;
  FILE *f;
  char buf[1024];
  char *p;

  // utime and stime are fields 14 and 15 of /proc/self/stat, after the command
  // name which is in parentheses and may contain spaces
  f = fopen("/proc/self/stat", "r");
  if (f)
    {
      if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')')))
	sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime);
      fclose(f);
    }

  f = fopen("/proc/self/statm", "r");
  if (f)
    {
      if (fscanf(f, "%lu %lu", &vsize_pages, &rss_pages) != 2)
	rss_pages = 0;
      fclose(f);
    }

  json_object_object_add(proc, "pid", json_object_new_int(getpid()));
  json_object_object_add(proc, "uptime_s", json_object_new_int64(time(NULL) - stats_process_start));
  // Total CPU time used, the client gets the CPU percentage from two samples
  json_object_object_add(proc, "cpu_time_ms", json_object_new_int64(ticks > 0 ? (int64_t)(utime + stime) * 1000 / ticks : 0));
  json_object_object_add(proc, "rss_kb", json_object_new_int64((int64_t)rss_pages * page / 1024));
  json_object_object_add(obj, "process", proc);
}

static void
host_json_add(json_object *obj)
{
  json_object *host = json_object_new_object();
  double load[3] = { 0 };
  long mem_total = 0;
  long mem_avail = 0;
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  FILE *f;
  char line[128];

  if (getloadavg(load, 3) == 3)
    {
      json_object_object_add(host, "load_1m", json_object_new_double(load[0]));
      json_object_object_add(host, "load_5m", json_object_new_double(load[1]));
      json_object_object_add(host, "load_15m", json_object_new_double(load[2]));
    }

  f = fopen("/proc/meminfo", "r");
  if (f)
    {
      while (fgets(line, sizeof(line), f))
	{
	  sscanf(line, "MemTotal: %ld kB", &mem_total);
	  sscanf(line, "MemAvailable: %ld kB", &mem_avail);
	}
      fclose(f);
    }

  json_object_object_add(host, "cpus", json_object_new_int(cpus > 0 ? cpus : 1));
  if (mem_total)
    {
      json_object_object_add(host, "mem_total_kb", json_object_new_int64(mem_total));
      json_object_object_add(host, "mem_available_kb", json_object_new_int64(mem_avail));
    }
  json_object_object_add(obj, "host", host);
}


/* ---------------------------------- JSON ----------------------------------- */

void
stats_json_add(json_object *obj)
{
  json_object *player = json_object_new_object();
  json_object *outputs = json_object_new_object();
  json_object *o;
  struct timespec now;
  char key[32];
  int i;
  int c;

  clock_gettime(CLOCK_MONOTONIC, &now);
  json_object_object_add(obj, "now_ms", json_object_new_int64((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000));

  process_json_add(obj);
  host_json_add(obj);

  pthread_mutex_lock(&stats_lck);

  for (c = 0; c < STATS_PLAYER_COUNTERS; c++)
    json_object_object_add(player, stats_player_names[c], json_object_new_int64(stats_player[c]));

  for (i = 0; i < STATS_MAX_OUTPUTS; i++)
    {
      if (!stats_outputs[i].in_use)
	continue;

      o = json_object_new_object();
      for (c = 0; c < STATS_OUTPUT_COUNTERS; c++)
	json_object_object_add(o, stats_output_names[c], json_object_new_int64(stats_outputs[i].counters[c]));
      if (stats_outputs[i].last_event[0])
	{
	  json_object_object_add(o, "last_event", json_object_new_string(stats_outputs[i].last_event));
	  json_object_object_add(o, "last_event_age_s", json_object_new_int64(time(NULL) - stats_outputs[i].last_event_time));
	}
      json_object_object_add(o, "transport", json_object_new_string(stats_outputs[i].transport));
      json_object_object_add(o, "eq_active", json_object_new_boolean(stats_outputs[i].eq_active));
      json_object_object_add(o, "encoder_sessions", json_object_new_int(stats_outputs[i].encoder_sessions));

      snprintf(key, sizeof(key), "%" PRIu64, stats_outputs[i].device_id);
      json_object_object_add(outputs, key, o);
    }

  pthread_mutex_unlock(&stats_lck);

  json_object_object_add(obj, "player", player);
  json_object_object_add(obj, "outputs", outputs);
}
