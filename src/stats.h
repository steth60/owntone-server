#ifndef __STATS_H__
#define __STATS_H__

#include <stdbool.h>
#include <stdint.h>
#include <json.h>

/* Performance and sync counters for the monitoring page (GET /api/stats).
 * Counters only ever increase; clients compute rates from two snapshots.
 * All functions are thread safe and cheap enough to call per packet.
 */

enum stats_output_counter
{
  STATS_OUTPUT_PACKETS = 0,         // Audio packets sent
  STATS_OUTPUT_BYTES,               // Audio bytes sent
  STATS_OUTPUT_SEND_ERRORS,         // Packets that could not be sent
  STATS_OUTPUT_RETRANSMIT_REQUESTS, // Requests from the speaker for lost packets
  STATS_OUTPUT_RETRANSMITS,         // Packets sent again
  STATS_OUTPUT_RETRANSMITS_MISSED,  // Requested packets we no longer had
  STATS_OUTPUT_SESSIONS,            // Sessions started
  STATS_OUTPUT_FAILURES,            // Sessions that failed
  STATS_OUTPUT_DSP_CLIPS,           // Samples clipped by the speaker's EQ
  STATS_OUTPUT_COUNTERS,
};

enum stats_player_counter
{
  STATS_PLAYER_STARTS = 0,
  STATS_PLAYER_UNDERRUNS,           // Source didn't deliver data in time
  STATS_PLAYER_FLUSHES,
  STATS_PLAYER_READ_ERRORS,
  STATS_PLAYER_COUNTERS,
};

void
stats_output_add(uint64_t device_id, enum stats_output_counter counter, uint64_t n);

// Records what happened last to the device, e.g. "session failed"
void
stats_output_event(uint64_t device_id, const char *event);

// Extra info about how the device is fed (shown on the monitoring page)
void
stats_output_session_set(uint64_t device_id, const char *transport, bool eq_active, int encoder_sessions);

void
stats_player_add(enum stats_player_counter counter, uint64_t n);

// Fills obj with "process", "host", "player" counters and "outputs" (an
// object keyed by device id as a string)
void
stats_json_add(json_object *obj);

#endif /* !__STATS_H__ */
