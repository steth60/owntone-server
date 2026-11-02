#ifndef __AIRPTP_H__
#define __AIRPTP_H__

#include <inttypes.h>

/* libairptp supports three modes of operation:
 * - Running a shared, standalone ptp daemon. In this case you will bind the ptp
 *   ports with airptp_daemon_bind() and then start with airptp_daemon_start()
 *   where is_shared is true.
 * - Running a private ptp daemon. You still bind and start (with is_shared
 *   false), but other processes using libairptpd won't be able to find it.
 * - As a library facilitating making requests to a ptp daemon, shared or
 *   private.
 */

struct airptp_handle;

struct airptp_callbacks
{
  // Optional - set name of thread
  void (*thread_name_set)(const char *name);

  // Debugging
  void (*hexdump)(const char *msg, uint8_t *data, size_t data_len);
  void (*logmsg)(const char *fmt, ...);
};

void
airptp_callbacks_register(struct airptp_callbacks *cb);

// Returns a handle if it was possible to bind to port 319 and 320. This
// normally requires root privilies. If node is non-NULL will bind to that
// specific address.
struct airptp_handle *
airptp_daemon_bind(const char *node);

// Starts a PTP daemon. Ports must have been bound already. Starting the daemon
// does not require privileges.
int
airptp_daemon_start(struct airptp_handle *hdl, uint64_t clock_id_seed, bool is_shared);

// Returns a handle if the host is running a compatible airptp daemon.
struct airptp_handle *
airptp_daemon_find(void);

int
airptp_peer_add(uint32_t *peer_id, const char *addr, struct airptp_handle *hdl);

void
airptp_peer_remove(uint32_t peer_id, struct airptp_handle *hdl);

// Frees ressources (incl. stops daemon if relevant)
void
airptp_end(struct airptp_handle *hdl);

// exit_cb?

int
airptp_clock_id_get(uint64_t *clock_id, struct airptp_handle *hdl);

const char *
airptp_errmsg_get(void);

/* Clock following
 *
 * Besides serving its own clock, the daemon tracks the clocks of PTP masters
 * that send it (two-step) Sync messages, e.g. an AirPlay sender streaming to
 * us when we act as a receiver. Each such clock gets an estimate of how it
 * relates to our clock:
 *
 *   remote clock time = local CLOCK_MONOTONIC time + offset_ns
 *
 * The estimate ignores the network path delay (like nqptp), so remote time is
 * early by roughly the one-way delay, typically well under a millisecond on a
 * LAN. Only available when the daemon runs in this process.
 */
struct airptp_clock_info
{
  uint64_t clock_id;
  uint64_t grandmaster_id; // From the clock's Announce, 0 if none seen
  char address[64];
  int64_t offset_ns;
  uint64_t updated_ns;     // CLOCK_MONOTONIC time of the last update
  uint64_t samples;        // Sync/Follow Up pairs used
  uint64_t steps;          // Times the estimate was reset after a jump
  double jitter_us;        // Average amount samples arrive late
  double drift_ppm;        // Remote clock rate relative to ours
};

// Gets the estimate for clock_id. With clock_id 0 returns the most recently
// updated clock. Returns 0 on success, -1 if there is no such clock (or no
// estimate yet).
int
airptp_clock_info_get(struct airptp_clock_info *info, uint64_t clock_id, struct airptp_handle *hdl);

// Fills up to max entries with all tracked clocks, returns the number filled
int
airptp_clocks_get(struct airptp_clock_info *info, int max, struct airptp_handle *hdl);

// By default airptp uses ports 319 and 320 as set by the standard, but for
// testing you can override that here.
void
airptp_ports_override(unsigned short event_port, unsigned short general_port);

#endif // __AIRPTP_H__
