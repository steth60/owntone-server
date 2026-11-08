#ifndef __AIRPLAY_RECEIVER_H__
#define __AIRPLAY_RECEIVER_H__

#include <stdbool.h>

/* Native AirPlay 2 receiver. Advertises OwnTone as an AirPlay 2 speaker so
 * senders like Music.app or an iPhone can stream to it directly, without an
 * external receiver (e.g. Shairport Sync) writing to a pipe.
 *
 * Phase 1: discovery, /info, transient HomeKit pairing, encrypted control
 * channel and the two-stage SETUP. Audio is accepted and counted, not played.
 */

// Thread: main. Returns 0 also if the receiver is disabled in the config.
int
airplay_receiver_init(void);

// Thread: main
void
airplay_receiver_deinit(void);

struct json_object;

// Adds a "receiver" object with the stream and sender clock state (for the
// monitoring page). Thread: any
void
airplay_receiver_stats_json_add(struct json_object *obj);

// True if name is the name the receiver advertises, so the AirPlay output
// module can avoid adding OwnTone's own receiver as a speaker.
bool
airplay_receiver_is_self(const char *name);

// True unless a sender is streaming to us and playing (rate > 0), so a paused
// AirPlay stream counts as idle. Thread: any
bool
airplay_receiver_is_idle(void);

#endif /* !__AIRPLAY_RECEIVER_H__ */
