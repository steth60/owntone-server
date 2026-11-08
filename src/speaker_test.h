#ifndef __SPEAKER_TEST_H__
#define __SPEAKER_TEST_H__

#include <stdint.h>
#include <json.h>

/* Speaker test: plays a short test tone on one speaker alone, through its
 * EQ, and reports whether it connected, received the audio and
 * stayed connected. Only runs while nothing is playing, and puts the speaker
 * selection back afterwards. One test at a time.
 */

enum speaker_test_result
{
  SPEAKER_TEST_STARTED = 0,
  SPEAKER_TEST_ERR_UNKNOWN_SPEAKER,
  SPEAKER_TEST_ERR_NOT_IDLE,  // Something is playing
  SPEAKER_TEST_ERR_BUSY,      // Another test is running
  SPEAKER_TEST_ERR_INTERNAL,
};

enum speaker_test_result
speaker_test_start(uint64_t speaker_id);

// Fills obj with the state of the last test: "speaker_id", "state" (idle,
// running, passed, failed), "message" and "packets"
void
speaker_test_json_add(json_object *obj);

#endif /* !__SPEAKER_TEST_H__ */
