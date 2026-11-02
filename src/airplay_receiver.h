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

// True if name is the name the receiver advertises, so the AirPlay output
// module can avoid adding OwnTone's own receiver as a speaker.
bool
airplay_receiver_is_self(const char *name);

#endif /* !__AIRPLAY_RECEIVER_H__ */
