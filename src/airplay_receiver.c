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
 * Native AirPlay 2 receiver, phase 1.
 *
 * What a sender (Music.app, iOS) does, and what this module answers:
 *
 *  1. Finds us via mDNS (_airplay._tcp and _raop._tcp)
 *  2. GET /info                        -> binary plist describing the device
 *  3. POST /pair-setup (transient)     -> HomeKit SRP with the fixed PIN, gives
 *                                         a shared secret; from the next request
 *                                         on the RTSP channel is encrypted
 *  4. POST /fp-setup (some senders)    -> fixed FairPlay replies
 *  5. SETUP #1 (timingProtocol PTP)    -> event port + our timing peer info
 *  6. SETUP #2 (streams, type 96/103)  -> data port, control port, buffer size
 *  7. SETPEERS, RECORD, SETRATEANCHORTIME, FLUSHBUFFERED, SET_PARAMETER,
 *     GET_PARAMETER, POST /feedback, TEARDOWN
 *
 * Phase 1 accepts the audio connections and counts what arrives. Clock
 * following (PTP), decryption, decoding and playback come in later phases.
 *
 * Protocol behaviour was checked against Shairport Sync's rtsp.c, which uses
 * the same pair_ap library for the server side of pairing.
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
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <gcrypt.h>
#include <sodium.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include <json.h>

#include "outputs/plist_wrap.h"
#include "pair_ap/pair.h"

#include "airplay_receiver.h"
#include "conffile.h"
#include "logger.h"
#include "misc.h"
#include "mdns.h"
#include "commands.h"
#include "db.h"
#include "input.h"
#include "outputs.h"
#include "player.h"
#include "worker.h"
#include "ptpd.h"
#include "libairptp/airptp.h"

// Values advertised by the sender-compatible receiver we model ourselves on
// (Shairport Sync 5.6 in AirPlay 2 mode). Music.app accepts these.
#define RX_SRCVERS              "366.0"
#define RX_FEATURES_LO          0x401C4A00
#define RX_FEATURES_HI          0x5C340
#define RX_FEATURES_STR         "0x401C4A00,0x5C340"
#define RX_FEATURES_EX          "AEocQEDDBQA"
#define RX_STATUS_FLAGS         0x4
#define RX_MODEL                "OwnTone"
#define RX_AUDIO_BUFFER_SIZE    8388608
#define RX_INITIAL_VOLUME       -24.0

#define RX_HEADER_MAX           65536
#define RX_QUEUE_MAX_BYTES      (16 * 1024 * 1024)
#define RX_SEQ_MASK             0x7FFFFF

// Format of a buffered audio packet, given by its SSRC field (as in Shairport
// Sync). OwnTone's own buffered sender uses RX_SSRC_AAC_44100 too.
#define RX_SSRC_NONE            0x00000000
#define RX_SSRC_ALAC_48000_S24  0x15000000
#define RX_SSRC_AAC_44100       0x16000000
#define RX_SSRC_AAC_48000       0x17000000
#define RX_AAC_FRAME_SAMPLES    1024
// Realtime packets don't carry a format in their SSRC. They are always ALAC
// 44100/16/2 (the only audioStream format we advertise), so this audioFormat
// bit stands in for the SSRC.
#define RX_FMT_ALAC_44100_S16   0x00040000
#define RX_ALAC_FRAME_SAMPLES   352
// The anchor in a realtime sync packet is this many frames later than the
// frame it names, see Shairport Sync's rtp_ap2_control_receiver()
#define RX_REALTIME_ANCHOR_FUDGE 11025
// Hold back this much realtime audio so packets that arrive out of order
// still get in line
#define RX_REALTIME_REORDER_FRAMES 4410
// A gap in realtime audio up to this long is filled with silence, a longer
// one makes the input find its place on the sender's timeline again
#define RX_REALTIME_GAP_MAX_FRAMES 44100
// A realtime sync packet that moves the timeline by more than this resyncs
#define RX_REALTIME_RESYNC_NS   20000000LL
#define RX_BODY_MAX             (4 * 1024 * 1024)
#define RX_UUID_LEN             37
#define RX_ADDR_LEN             (INET6_ADDRSTRLEN + 8)

enum rx_service_kind
{
  RX_SERVICE_EVENT,
  RX_SERVICE_DATA,
  RX_SERVICE_CONTROL,
};

struct rx_session;

// A per-session port the sender connects (TCP) or sends (UDP) to
struct rx_service
{
  enum rx_service_kind kind;
  const char *label;
  int type; // SOCK_STREAM or SOCK_DGRAM
  struct net_socket sock;
  unsigned short port;

  struct evconnlistener *listener4;
  struct evconnlistener *listener6;
  struct bufferevent *bev; // The accepted TCP connection
  struct event *udp_ev4;
  struct event *udp_ev6;

  struct rx_session *session;

  uint64_t bytes;
  uint64_t packets;
};

struct rx_session
{
  int id;
  struct bufferevent *bev;
  struct evbuffer *plain_in;

  char peer_addr[RX_ADDR_LEN];
  char local_addr[RX_ADDR_LEN];

  struct pair_setup_context *setup_ctx;
  struct pair_verify_context *verify_ctx;
  struct pair_cipher_context *cipher;
  uint8_t shared_secret[64];
  size_t shared_secret_len;
  bool encrypted;

  char *client_name;
  char *timing_protocol;
  char *group_uuid;
  int stream_type;

  // Latest SETRATEANCHORTIME, converted to our clock (phase 3 plays from it)
  bool anchor_valid;
  uint64_t anchor_rate;
  uint32_t anchor_rtptime;
  uint64_t anchor_clock_id;
  uint64_t anchor_local_ns; // CLOCK_MONOTONIC time when anchor_rtptime plays
  int feedback_count;

  struct rx_service *event_svc;
  struct rx_service *data_svc;
  struct rx_service *control_svc;

  struct rx_session *next;
};

struct rx_request
{
  char method[32];
  char path[512];
  struct keyval headers;
  uint8_t *body;
  size_t body_len;
  plist_t plist; // Parsed body if it was a binary plist
};

struct rx_response
{
  int code;
  struct keyval headers;
  uint8_t *body;
  size_t body_len;
};

struct rx_packet
{
  uint32_t seq;
  uint32_t rtptime;
  uint32_t ssrc;
  uint8_t *data; // Decrypted AAC frame
  size_t len;
  struct rx_packet *next;
};

// The audio stream from the current sender. Written by the receiver thread,
// read by the player's input thread (see the input at the end of this file).
struct rx_stream
{
  pthread_mutex_t lck;

  bool active;
  bool ended;
  bool realtime; // Type 96 over UDP, else type 103 over TCP
  int session_id;
  char *client_name;
  uint8_t key[32];
  bool has_key;

  struct rx_packet *head;
  struct rx_packet *tail;
  size_t queued_bytes;

  // Changes whenever the input must find its place again (new anchor, flush)
  uint32_t generation;
  bool anchor_valid;
  uint64_t rate;
  uint32_t anchor_rtptime;
  uint64_t anchor_local_ns;

  // Packets up to (not including) this sequence number are dropped on arrival
  bool flush_active;
  uint32_t flush_until_seq;
  // Realtime: packets before this RTP time are dropped on arrival (FLUSH)
  bool flush_rtp_active;
  uint32_t flush_until_rtptime;
  // Deferred flush of a range further ahead
  bool flush_range_active;
  uint32_t flush_from_seq;

  uint64_t packets_in;
  uint64_t packets_bad;
  uint64_t packets_dropped;
  // Frames skipped because they were due before the outputs could play them
  uint64_t late_frames_dropped; // While syncing to the current anchor
  uint64_t late_ms_total;
};

static struct rx_stream rx_stream = { .lck = PTHREAD_MUTEX_INITIALIZER };
static uint32_t rx_queue_item_id;

static bool rx_enabled;
static struct event_base *evbase_rx;
static struct commands_base *cmdbase;
static pthread_t tid_rx;
static struct net_socket rx_socket;
static struct evconnlistener *rx_listener4;
static struct evconnlistener *rx_listener6;
static struct rx_session *rx_sessions;
static int rx_session_counter;

static char rx_name[128];
static char rx_raop_name[160];
static char rx_device_id[18];     // "AA:BB:CC:DD:EE:FF"
static char rx_device_id_raop[13]; // "AABBCCDDEEFF"
static char rx_pi[RX_UUID_LEN];
static char rx_psi[RX_UUID_LEN];
static char rx_pgid[RX_UUID_LEN];
static uint8_t rx_pk[32];
static char rx_pk_hex[65];
static unsigned short rx_port;


/* --------------------------- FairPlay setup ------------------------------- */

// Fixed replies to the FairPlay v3 setup messages, as used by every open
// AirPlay receiver. The sender only checks that the exchange completes.

static const uint8_t server_fp_reply1[] =
    "\x46\x50\x4c\x59\x03\x01\x02\x00\x00\x00\x00\x82\x02\x00\x0f\x9f\x3f\x9e\x0a"
    "\x25\x21\xdb\xdf\x31\x2a\xb2\xbf\xb2\x9e\x8d\x23\x2b\x63\x76\xa8\xc8\x18\x70"
    "\x1d\x22\xae\x93\xd8\x27\x37\xfe\xaf\x9d\xb4\xfd\xf4\x1c\x2d\xba\x9d\x1f\x49"
    "\xca\xaa\xbf\x65\x91\xac\x1f\x7b\xc6\xf7\xe0\x66\x3d\x21\xaf\xe0\x15\x65\x95"
    "\x3e\xab\x81\xf4\x18\xce\xed\x09\x5a\xdb\x7c\x3d\x0e\x25\x49\x09\xa7\x98\x31"
    "\xd4\x9c\x39\x82\x97\x34\x34\xfa\xcb\x42\xc6\x3a\x1c\xd9\x11\xa6\xfe\x94\x1a"
    "\x8a\x6d\x4a\x74\x3b\x46\xc3\xa7\x64\x9e\x44\xc7\x89\x55\xe4\x9d\x81\x55\x00"
    "\x95\x49\xc4\xe2\xf7\xa3\xf6\xd5\xba";
static const uint8_t server_fp_reply2[] =
    "\x46\x50\x4c\x59\x03\x01\x02\x00\x00\x00\x00\x82\x02\x01\xcf\x32\xa2\x57\x14"
    "\xb2\x52\x4f\x8a\xa0\xad\x7a\xf1\x64\xe3\x7b\xcf\x44\x24\xe2\x00\x04\x7e\xfc"
    "\x0a\xd6\x7a\xfc\xd9\x5d\xed\x1c\x27\x30\xbb\x59\x1b\x96\x2e\xd6\x3a\x9c\x4d"
    "\xed\x88\xba\x8f\xc7\x8d\xe6\x4d\x91\xcc\xfd\x5c\x7b\x56\xda\x88\xe3\x1f\x5c"
    "\xce\xaf\xc7\x43\x19\x95\xa0\x16\x65\xa5\x4e\x19\x39\xd2\x5b\x94\xdb\x64\xb9"
    "\xe4\x5d\x8d\x06\x3e\x1e\x6a\xf0\x7e\x96\x56\x16\x2b\x0e\xfa\x40\x42\x75\xea"
    "\x5a\x44\xd9\x59\x1c\x72\x56\xb9\xfb\xe6\x51\x38\x98\xb8\x02\x27\x72\x19\x88"
    "\x57\x16\x50\x94\x2a\xd9\x46\x68\x8a";
static const uint8_t server_fp_reply3[] =
    "\x46\x50\x4c\x59\x03\x01\x02\x00\x00\x00\x00\x82\x02\x02\xc1\x69\xa3\x52\xee"
    "\xed\x35\xb1\x8c\xdd\x9c\x58\xd6\x4f\x16\xc1\x51\x9a\x89\xeb\x53\x17\xbd\x0d"
    "\x43\x36\xcd\x68\xf6\x38\xff\x9d\x01\x6a\x5b\x52\xb7\xfa\x92\x16\xb2\xb6\x54"
    "\x82\xc7\x84\x44\x11\x81\x21\xa2\xc7\xfe\xd8\x3d\xb7\x11\x9e\x91\x82\xaa\xd7"
    "\xd1\x8c\x70\x63\xe2\xa4\x57\x55\x59\x10\xaf\x9e\x0e\xfc\x76\x34\x7d\x16\x40"
    "\x43\x80\x7f\x58\x1e\xe4\xfb\xe4\x2c\xa9\xde\xdc\x1b\x5e\xb2\xa3\xaa\x3d\x2e"
    "\xcd\x59\xe7\xee\xe7\x0b\x36\x29\xf2\x2a\xfd\x16\x1d\x87\x73\x53\xdd\xb9\x9a"
    "\xdc\x8e\x07\x00\x6e\x56\xf8\x50\xce";
static const uint8_t server_fp_reply4[] =
    "\x46\x50\x4c\x59\x03\x01\x02\x00\x00\x00\x00\x82\x02\x03\x90\x01\xe1\x72\x7e"
    "\x0f\x57\xf9\xf5\x88\x0d\xb1\x04\xa6\x25\x7a\x23\xf5\xcf\xff\x1a\xbb\xe1\xe9"
    "\x30\x45\x25\x1a\xfb\x97\xeb\x9f\xc0\x01\x1e\xbe\x0f\x3a\x81\xdf\x5b\x69\x1d"
    "\x76\xac\xb2\xf7\xa5\xc7\x08\xe3\xd3\x28\xf5\x6b\xb3\x9d\xbd\xe5\xf2\x9c\x8a"
    "\x17\xf4\x81\x48\x7e\x3a\xe8\x63\xc6\x78\x32\x54\x22\xe6\xf7\x8e\x16\x6d\x18"
    "\xaa\x7f\xd6\x36\x25\x8b\xce\x28\x72\x6f\x66\x1f\x73\x88\x93\xce\x44\x31\x1e"
    "\x4b\xe6\xc0\x53\x51\x93\xe5\xef\x72\xe8\x68\x62\x33\x72\x9c\x22\x7d\x82\x0c"
    "\x99\x94\x45\xd8\x92\x46\xc8\xc3\x59";

static const uint8_t server_fp_header[] = "\x46\x50\x4c\x59\x03\x01\x04\x00\x00\x00\x00\x14";


/* ------------------------------- Helpers ---------------------------------- */

static uint8_t *
memdup(const uint8_t *src, size_t len)
{
  uint8_t *out;

  CHECK_NULL(L_AIRPLAY_RX, out = malloc(len));
  memcpy(out, src, len);
  return out;
}

static void
uuid_from_seed(char *out, size_t out_len, const char *seed, const char *purpose)
{
  char buf[256];
  uint8_t h[20];

  snprintf(buf, sizeof(buf), "%s/%s", seed, purpose);
  gcry_md_hash_buffer(GCRY_MD_SHA1, h, buf, strlen(buf));

  // Shape it like a name based (v5) UUID
  h[6] = (h[6] & 0x0f) | 0x50;
  h[8] = (h[8] & 0x3f) | 0x80;

  snprintf(out, out_len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	   h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
}

static int
device_id_from_interfaces(char *out, size_t out_len)
{
  struct ifaddrs *ifap;
  struct ifaddrs *ifa;
  uint8_t mac[6];
  int ret = -1;

  if (getifaddrs(&ifap) < 0)
    return -1;

  for (ifa = ifap; ifa; ifa = ifa->ifa_next)
    {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
	continue;
      if ((ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP))
	continue;
      if (net_mac_get(mac, sizeof(mac), ifa->ifa_name) < 0)
	continue;
      if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0)
	continue;

      snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      DPRINTF(E_DBG, L_AIRPLAY_RX, "Using MAC of interface %s as device id\n", ifa->ifa_name);
      ret = 0;
      break;
    }

  freeifaddrs(ifap);
  return ret;
}

static int
identity_init(void)
{
  cfg_t *rxcfg = cfg_getsec(cfg, "airplay_receiver");
  const char *name = cfg_getstr(rxcfg, "name");
  const char *device_id = cfg_getstr(rxcfg, "device_id");
  int i;
  int j;

  snprintf(rx_name, sizeof(rx_name), "%s", name);

  if (device_id)
    snprintf(rx_device_id, sizeof(rx_device_id), "%s", device_id);
  else if (device_id_from_interfaces(rx_device_id, sizeof(rx_device_id)) < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not find a MAC address for the device id, set 'device_id' in the airplay_receiver config section\n");
      return -1;
    }

  for (i = 0, j = 0; rx_device_id[i] && j < (int)sizeof(rx_device_id_raop) - 1; i++)
    {
      if (rx_device_id[i] != ':')
	rx_device_id_raop[j++] = rx_device_id[i];
    }
  rx_device_id_raop[j] = '\0';

  snprintf(rx_raop_name, sizeof(rx_raop_name), "%s@%s", rx_device_id_raop, rx_name);

  uuid_from_seed(rx_pi, sizeof(rx_pi), rx_device_id, "pi");
  uuid_from_seed(rx_psi, sizeof(rx_psi), rx_device_id, "psi");
  uuid_from_seed(rx_pgid, sizeof(rx_pgid), rx_device_id, "pgid");

  // pair_ap derives the server's key pair from the device id, so the public
  // key stays stable as long as the device id does
  pair_public_key_get(PAIR_SERVER_HOMEKIT, rx_pk, rx_device_id);
  for (i = 0; i < 32; i++)
    snprintf(rx_pk_hex + 2 * i, 3, "%02x", rx_pk[i]);

  return 0;
}

static void
sockaddr_to_string(char *out, size_t out_len, struct sockaddr *sa)
{
  union net_sockaddr naddr = { 0 };
  const char *prefix = "::ffff:";

  out[0] = '\0';
  if (sa->sa_family == AF_INET)
    memcpy(&naddr.sin, sa, sizeof(struct sockaddr_in));
  else if (sa->sa_family == AF_INET6)
    memcpy(&naddr.sin6, sa, sizeof(struct sockaddr_in6));
  else
    return;

  net_address_get(out, out_len, &naddr);

  // IPv4 mapped IPv6 address, the sender wants the plain IPv4 address
  if (strncmp(out, prefix, strlen(prefix)) == 0)
    memmove(out, out + strlen(prefix), strlen(out) - strlen(prefix) + 1);
}

static const char *
plist_dict_get_string(plist_t dict, const char *key, char **out)
{
  plist_t item;

  *out = NULL;
  item = plist_dict_get_item(dict, key);
  if (item && plist_get_node_type(item) == PLIST_STRING)
    plist_get_string_val(item, out);

  return *out;
}

static int
plist_dict_get_uint_val(plist_t dict, const char *key, uint64_t *out)
{
  plist_t item;

  item = plist_dict_get_item(dict, key);
  if (!item || plist_get_node_type(item) != PLIST_UINT)
    return -1;

  plist_get_uint_val(item, out);
  return 0;
}

static void
plist_log(int severity, const char *what, plist_t plist)
{
  char *xml = NULL;
  uint32_t len = 0;

  if (!plist)
    return;

  plist_to_xml(plist, &xml, &len);
  if (xml)
    DPRINTF(severity, L_AIRPLAY_RX, "%s:\n%s", what, xml);
  free(xml);
}


/* ------------------------------ TXT records -------------------------------- */

// Returns a NULL terminated array of "key=value" strings, free with txt_free()
static char **
txt_airplay_make(bool for_info)
{
  char **txt = calloc(24, sizeof(char *));
  int n = 0;

  txt[n++] = safe_asprintf("acl=0");
  if (for_info)
    txt[n++] = safe_asprintf("btaddr=00:00:00:00:00:00");
  txt[n++] = safe_asprintf("deviceid=%s", rx_device_id);
  txt[n++] = safe_asprintf("fex=%s", RX_FEATURES_EX);
  txt[n++] = safe_asprintf("features=%s", RX_FEATURES_STR);
  txt[n++] = safe_asprintf("flags=0x%x", RX_STATUS_FLAGS);
  txt[n++] = safe_asprintf("gid=%s", rx_pi);
  txt[n++] = safe_asprintf("igl=0");
  txt[n++] = safe_asprintf("gcgl=0");
  if (for_info)
    txt[n++] = safe_asprintf("pgid=%s", rx_pgid);
  txt[n++] = safe_asprintf("model=%s", RX_MODEL);
  txt[n++] = safe_asprintf("protovers=1.1");
  txt[n++] = safe_asprintf("pi=%s", rx_pi);
  txt[n++] = safe_asprintf("psi=%s", rx_psi);
  txt[n++] = safe_asprintf("pk=%s", rx_pk_hex);
  txt[n++] = safe_asprintf("srcvers=%s", RX_SRCVERS);
  if (for_info)
    txt[n++] = safe_asprintf("vv=0");
  else
    txt[n++] = safe_asprintf("fv=%s", VERSION);
  txt[n] = NULL;

  return txt;
}

static char **
txt_raop_make(void)
{
  char **txt = calloc(16, sizeof(char *));
  int n = 0;

  txt[n++] = safe_asprintf("cn=0,1");
  txt[n++] = safe_asprintf("da=true");
  txt[n++] = safe_asprintf("et=0,1");
  txt[n++] = safe_asprintf("ft=%s", RX_FEATURES_STR);
  txt[n++] = safe_asprintf("fv=%s", VERSION);
  txt[n++] = safe_asprintf("sf=0x%x", RX_STATUS_FLAGS);
  txt[n++] = safe_asprintf("md=0,1,2");
  txt[n++] = safe_asprintf("am=%s", RX_MODEL);
  txt[n++] = safe_asprintf("pk=%s", rx_pk_hex);
  txt[n++] = safe_asprintf("tp=UDP");
  txt[n++] = safe_asprintf("vn=65537");
  txt[n++] = safe_asprintf("vs=%s", RX_SRCVERS);
  txt[n] = NULL;

  return txt;
}

static void
txt_free(char **txt)
{
  int i;

  if (!txt)
    return;

  for (i = 0; txt[i]; i++)
    free(txt[i]);
  free(txt);
}

// The txtAirPlay item in /info is the TXT record in DNS wire format: each
// string prefixed with its length
static uint8_t *
txt_wire_make(size_t *len)
{
  char **txt = txt_airplay_make(true);
  struct evbuffer *evbuf = evbuffer_new();
  uint8_t *out;
  size_t slen;
  int i;

  for (i = 0; txt[i]; i++)
    {
      slen = strlen(txt[i]);
      if (slen > 255)
	slen = 255;
      evbuffer_add(evbuf, (uint8_t[]){ (uint8_t)slen }, 1);
      evbuffer_add(evbuf, txt[i], slen);
    }

  *len = evbuffer_get_length(evbuf);
  out = malloc(*len);
  evbuffer_remove(evbuf, out, *len);

  evbuffer_free(evbuf);
  txt_free(txt);
  return out;
}


/* -------------------------- Per-session services --------------------------- */

static void
service_free(struct rx_service *svc)
{
  if (!svc)
    return;

  if (svc->bytes || svc->packets)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: %s port %hu received %" PRIu64 " bytes in %" PRIu64 " %s\n",
	    svc->session ? svc->session->id : -1, svc->label, svc->port, svc->bytes, svc->packets,
	    svc->type == SOCK_DGRAM ? "packets" : "reads");

  if (svc->bev)
    bufferevent_free(svc->bev);
  if (svc->listener4)
    evconnlistener_free(svc->listener4);
  if (svc->listener6)
    evconnlistener_free(svc->listener6);
  if (svc->udp_ev4)
    event_free(svc->udp_ev4);
  if (svc->udp_ev6)
    event_free(svc->udp_ev6);

  net_socket_close(&svc->sock);
  free(svc);
}

static void
service_tcp_read_cb(struct bufferevent *bev, void *arg)
{
  struct rx_service *svc = arg;
  struct evbuffer *input = bufferevent_get_input(bev);
  size_t len = evbuffer_get_length(input);

  // Phase 1: count and discard
  svc->bytes += len;
  svc->packets++;
  evbuffer_drain(input, len);
}

static void
service_tcp_event_cb(struct bufferevent *bev, short events, void *arg)
{
  struct rx_service *svc = arg;

  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    {
      DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: sender closed the %s connection\n", svc->session->id, svc->label);
      bufferevent_free(svc->bev);
      svc->bev = NULL;
    }
}

static void
buffered_read_cb(struct bufferevent *bev, void *arg);
static void
stream_packet_add(const uint8_t *p, size_t len, bool realtime);
static void
realtime_control_packet(struct rx_session *s, const uint8_t *p, size_t len);

static void
service_accept_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *address, int socklen, void *arg)
{
  struct rx_service *svc = arg;
  char addr[RX_ADDR_LEN];

  sockaddr_to_string(addr, sizeof(addr), address);
  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: %s connection from %s\n", svc->session->id, svc->label, addr);

  if (svc->bev)
    bufferevent_free(svc->bev);

  svc->bev = bufferevent_socket_new(evbase_rx, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!svc->bev)
    {
      close(fd);
      return;
    }

  if (svc->kind == RX_SERVICE_DATA && svc->session->stream_type == 103)
    bufferevent_setcb(svc->bev, buffered_read_cb, NULL, service_tcp_event_cb, svc);
  else
    bufferevent_setcb(svc->bev, service_tcp_read_cb, NULL, service_tcp_event_cb, svc);
  bufferevent_enable(svc->bev, EV_READ);
}

static void
service_udp_read_cb(evutil_socket_t fd, short what, void *arg)
{
  struct rx_service *svc = arg;
  uint8_t buf[4096];
  ssize_t len;

  while ((len = recv(fd, buf, sizeof(buf), 0)) > 0)
    {
      svc->bytes += len;
      svc->packets++;

      if (svc->session->stream_type != 96)
	continue;

      if (svc->kind == RX_SERVICE_DATA)
	stream_packet_add(buf, len, true);
      else if (svc->kind == RX_SERVICE_CONTROL)
	realtime_control_packet(svc->session, buf, len);
    }
}

static struct rx_service *
service_new(struct rx_session *session, enum rx_service_kind kind, int type, const char *label)
{
  struct rx_service *svc;
  int ret;

  CHECK_NULL(L_AIRPLAY_RX, svc = calloc(1, sizeof(struct rx_service)));
  svc->kind = kind;
  svc->type = type;
  svc->label = label;
  svc->session = session;
  svc->sock.fd4 = -1;
  svc->sock.fd6 = -1;

  ret = net_bind(&svc->sock, &svc->port, type, label);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not open %s port\n", session->id, label);
      free(svc);
      return NULL;
    }

  if (type == SOCK_STREAM)
    {
      if (svc->sock.fd4 >= 0)
	svc->listener4 = evconnlistener_new(evbase_rx, service_accept_cb, svc, 0, -1, svc->sock.fd4);
      if (svc->sock.fd6 >= 0)
	svc->listener6 = evconnlistener_new(evbase_rx, service_accept_cb, svc, 0, -1, svc->sock.fd6);
    }
  else
    {
      if (svc->sock.fd4 >= 0)
	{
	  evutil_make_socket_nonblocking(svc->sock.fd4);
	  svc->udp_ev4 = event_new(evbase_rx, svc->sock.fd4, EV_READ | EV_PERSIST, service_udp_read_cb, svc);
	  event_add(svc->udp_ev4, NULL);
	}
      if (svc->sock.fd6 >= 0)
	{
	  evutil_make_socket_nonblocking(svc->sock.fd6);
	  svc->udp_ev6 = event_new(evbase_rx, svc->sock.fd6, EV_READ | EV_PERSIST, service_udp_read_cb, svc);
	  event_add(svc->udp_ev6, NULL);
	}
    }

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: %s listening on %s port %hu\n", session->id, label,
	  type == SOCK_STREAM ? "TCP" : "UDP", svc->port);
  return svc;
}

static void
session_streams_stop(struct rx_session *s)
{
  service_free(s->data_svc);
  s->data_svc = NULL;
  service_free(s->control_svc);
  s->control_svc = NULL;
  s->stream_type = 0;
}


/* ------------------------------ Audio stream ------------------------------- */
/*                 Receiver thread writes, input thread reads                   */

static inline uint32_t
be32_get(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Sequence numbers are 23 bit and wrap
static inline bool
seq_before(uint32_t a, uint32_t b)
{
  uint32_t diff = (a - b) & RX_SEQ_MASK;
  return diff != 0 && diff >= 0x400000;
}

static inline uint64_t
be64_get(const uint8_t *p)
{
  return ((uint64_t)be32_get(p) << 32) | be32_get(p + 4);
}

static int
ssrc_sample_rate(uint32_t ssrc)
{
  return (ssrc == RX_SSRC_AAC_48000 || ssrc == RX_SSRC_ALAC_48000_S24) ? 48000 : 44100;
}

static int
ssrc_frame_samples(uint32_t ssrc)
{
  return (ssrc == RX_FMT_ALAC_44100_S16) ? RX_ALAC_FRAME_SAMPLES : RX_AAC_FRAME_SAMPLES;
}

static void
packet_free(struct rx_packet *pkt)
{
  if (!pkt)
    return;
  free(pkt->data);
  free(pkt);
}

// Caller must hold rx_stream.lck
static void
stream_drop_head_locked(void)
{
  struct rx_packet *pkt = rx_stream.head;

  if (!pkt)
    return;

  rx_stream.head = pkt->next;
  if (!rx_stream.head)
    rx_stream.tail = NULL;
  rx_stream.queued_bytes -= pkt->len;
  packet_free(pkt);
}

// Caller must hold rx_stream.lck
static void
stream_clear_locked(void)
{
  while (rx_stream.head)
    stream_drop_head_locked();

  rx_stream.anchor_valid = false;
  rx_stream.flush_active = false;
  rx_stream.flush_range_active = false;
  rx_stream.flush_rtp_active = false;
  rx_stream.generation++;
}

static void
stream_start(struct rx_session *s, const uint8_t *key, size_t key_len)
{
  pthread_mutex_lock(&rx_stream.lck);

  stream_clear_locked();
  rx_stream.active = true;
  rx_stream.ended = false;
  rx_stream.realtime = (s->stream_type == 96);
  rx_stream.session_id = s->id;
  free(rx_stream.client_name);
  rx_stream.client_name = s->client_name ? strdup(s->client_name) : NULL;
  rx_stream.has_key = (key_len == sizeof(rx_stream.key));
  if (rx_stream.has_key)
    memcpy(rx_stream.key, key, sizeof(rx_stream.key));
  rx_stream.rate = 0;
  rx_stream.packets_in = 0;
  rx_stream.packets_bad = 0;
  rx_stream.packets_dropped = 0;

  pthread_mutex_unlock(&rx_stream.lck);

  if (!rx_stream.has_key)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: stream SETUP without a valid session key, audio can't be decrypted\n", s->id);
}

static void
stream_end(int session_id, const char *why)
{
  pthread_mutex_lock(&rx_stream.lck);

  if (rx_stream.active && rx_stream.session_id == session_id)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: audio stream ended (%s), %" PRIu64 " packets in, %" PRIu64 " undecryptable, %" PRIu64 " dropped\n",
	      session_id, why, rx_stream.packets_in, rx_stream.packets_bad, rx_stream.packets_dropped);
      stream_clear_locked();
      rx_stream.active = false;
      rx_stream.ended = true;
      sodium_memzero(rx_stream.key, sizeof(rx_stream.key));
      rx_stream.has_key = false;
    }

  pthread_mutex_unlock(&rx_stream.lck);
}

static void
stream_anchor_set(uint64_t rate, bool has_anchor, uint32_t rtptime, uint64_t local_ns)
{
  pthread_mutex_lock(&rx_stream.lck);

  rx_stream.rate = rate;
  if (has_anchor)
    {
      rx_stream.anchor_valid = true;
      rx_stream.anchor_rtptime = rtptime;
      rx_stream.anchor_local_ns = local_ns;
      rx_stream.generation++;
    }

  pthread_mutex_unlock(&rx_stream.lck);
}

// Without from_seq: drop everything before until_seq, now and on arrival.
// With from_seq: drop the range [from_seq, until_seq) when it arrives (the
// sender does this to skip ahead while the current audio plays on).
static void
stream_flush(bool has_from, uint32_t from_seq, uint32_t until_seq)
{
  struct rx_packet *prev = NULL;
  struct rx_packet *pkt;
  struct rx_packet *next;

  pthread_mutex_lock(&rx_stream.lck);

  if (!has_from)
    {
      while (rx_stream.head && seq_before(rx_stream.head->seq, until_seq))
	{
	  stream_drop_head_locked();
	  rx_stream.packets_dropped++;
	}

      rx_stream.flush_active = true;
      rx_stream.flush_until_seq = until_seq;
      rx_stream.anchor_valid = false; // A new anchor will follow
      rx_stream.generation++;
    }
  else
    {
      for (pkt = rx_stream.head; pkt; pkt = next)
	{
	  next = pkt->next;
	  if (seq_before(pkt->seq, from_seq) || !seq_before(pkt->seq, until_seq))
	    {
	      prev = pkt;
	      continue;
	    }

	  if (prev)
	    prev->next = next;
	  else
	    rx_stream.head = next;
	  if (rx_stream.tail == pkt)
	    rx_stream.tail = prev;
	  rx_stream.queued_bytes -= pkt->len;
	  rx_stream.packets_dropped++;
	  packet_free(pkt);
	}

      rx_stream.flush_range_active = true;
      rx_stream.flush_from_seq = from_seq;
      rx_stream.flush_until_seq = until_seq;
    }

  pthread_mutex_unlock(&rx_stream.lck);
}

// Realtime FLUSH: drop everything before rtptime, now and on arrival, then
// find our place on the sender's timeline again
static void
stream_flush_rtptime(uint32_t rtptime)
{
  pthread_mutex_lock(&rx_stream.lck);

  while (rx_stream.head && (int32_t)(rx_stream.head->rtptime - rtptime) < 0)
    {
      stream_drop_head_locked();
      rx_stream.packets_dropped++;
    }

  rx_stream.flush_rtp_active = true;
  rx_stream.flush_until_rtptime = rtptime;
  rx_stream.generation++;

  pthread_mutex_unlock(&rx_stream.lck);
}

// Caller must hold rx_stream.lck. Returns true if the packet must be dropped
static bool
stream_flush_check_locked(uint32_t seq)
{
  if (rx_stream.flush_active)
    {
      if (seq_before(seq, rx_stream.flush_until_seq))
	return true;
      rx_stream.flush_active = false;
    }

  if (rx_stream.flush_range_active)
    {
      if (!seq_before(seq, rx_stream.flush_until_seq))
	rx_stream.flush_range_active = false;
      else if (!seq_before(seq, rx_stream.flush_from_seq))
	return true;
    }

  return false;
}

// Caller must hold rx_stream.lck. Realtime packets come over UDP, so they can
// arrive out of order or twice. They are kept in RTP time order, and the
// input holds some back (RX_REALTIME_REORDER_FRAMES) so late ones fit in.
// Returns false if the packet is a duplicate.
static bool
stream_realtime_insert_locked(struct rx_packet *pkt)
{
  struct rx_packet *prev = NULL;
  struct rx_packet *cur;

  if (!rx_stream.tail || (int32_t)(pkt->rtptime - rx_stream.tail->rtptime) > 0)
    {
      if (rx_stream.tail)
	rx_stream.tail->next = pkt;
      else
	rx_stream.head = pkt;
      rx_stream.tail = pkt;
      return true;
    }

  for (cur = rx_stream.head; cur && (int32_t)(pkt->rtptime - cur->rtptime) > 0; cur = cur->next)
    prev = cur;

  if (cur && cur->rtptime == pkt->rtptime)
    return false;

  pkt->next = cur;
  if (prev)
    prev->next = pkt;
  else
    rx_stream.head = pkt;
  return true;
}

// An audio packet: 4 bytes marker + sequence number, 4 bytes RTP timestamp,
// 4 bytes SSRC, then the ChaCha20-Poly1305 ciphertext with its 16 byte tag,
// then the last 8 bytes of the 12 byte nonce. The timestamp and SSRC are the
// additional authenticated data. Buffered packets have a 23 bit sequence
// number and the format in the SSRC. Realtime packets are plain RTP (16 bit
// sequence number) and always ALAC.
static void
stream_packet_add(const uint8_t *p, size_t len, bool realtime)
{
  struct rx_packet *pkt;
  uint8_t nonce[12] = { 0 };
  unsigned long long plain_len = 0;
  uint32_t seq;
  uint32_t ssrc;
  uint32_t rtptime;
  int ret;

  if (len < 12 + crypto_aead_chacha20poly1305_ietf_ABYTES + 8)
    return;

  rtptime = be32_get(p + 4);
  if (realtime)
    {
      seq = ((uint32_t)p[2] << 8) | p[3];
      ssrc = RX_FMT_ALAC_44100_S16;
    }
  else
    {
      seq = be32_get(p) & RX_SEQ_MASK;
      ssrc = be32_get(p + 8);
      if (ssrc == RX_SSRC_NONE)
	return; // Filler between formats, no audio
    }

  pthread_mutex_lock(&rx_stream.lck);

  rx_stream.packets_in++;

  if (!rx_stream.active || !rx_stream.has_key || rx_stream.realtime != realtime)
    goto drop;
  if (realtime && rx_stream.flush_rtp_active)
    {
      if ((int32_t)(rtptime - rx_stream.flush_until_rtptime) < 0)
	goto drop;
      rx_stream.flush_rtp_active = false;
    }
  if (!realtime && stream_flush_check_locked(seq))
    goto drop;

  CHECK_NULL(L_AIRPLAY_RX, pkt = calloc(1, sizeof(struct rx_packet)));
  CHECK_NULL(L_AIRPLAY_RX, pkt->data = malloc(len));
  memcpy(nonce + 4, p + len - 8, 8);

  ret = crypto_aead_chacha20poly1305_ietf_decrypt(pkt->data, &plain_len, NULL, p + 12, len - 12 - 8, p + 4, 8, nonce, rx_stream.key);
  if (ret != 0)
    {
      if (rx_stream.packets_bad++ < 5)
	DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not decrypt audio packet seq %u (%zu bytes)\n", seq, len);
      packet_free(pkt);
      pthread_mutex_unlock(&rx_stream.lck);
      return;
    }

  pkt->seq = seq;
  pkt->rtptime = rtptime;
  pkt->ssrc = ssrc;
  pkt->len = plain_len;

  if (realtime)
    {
      if (!stream_realtime_insert_locked(pkt))
	{
	  packet_free(pkt);
	  goto drop;
	}
    }
  else
    {
      if (rx_stream.tail)
	rx_stream.tail->next = pkt;
      else
	rx_stream.head = pkt;
      rx_stream.tail = pkt;
    }
  rx_stream.queued_bytes += pkt->len;

  // The sender respects the audioBufferSize we gave it, so this is only a
  // safeguard for when nobody plays the stream
  while (rx_stream.queued_bytes > RX_QUEUE_MAX_BYTES)
    {
      stream_drop_head_locked();
      rx_stream.packets_dropped++;
    }

  pthread_mutex_unlock(&rx_stream.lck);
  return;

 drop:
  rx_stream.packets_dropped++;
  pthread_mutex_unlock(&rx_stream.lck);
}

static void
buffered_read_cb(struct bufferevent *bev, void *arg)
{
  struct rx_service *svc = arg;
  struct evbuffer *input = bufferevent_get_input(bev);
  uint8_t hdr[2];
  uint8_t *p;
  size_t len;

  for (;;)
    {
      if (evbuffer_copyout(input, hdr, 2) < 2)
	break;

      // Length includes the two length bytes
      len = ((size_t)hdr[0] << 8) | hdr[1];
      if (len < 2)
	{
	  evbuffer_drain(input, 2);
	  continue;
	}
      if (evbuffer_get_length(input) < len)
	break;

      p = evbuffer_pullup(input, len);
      stream_packet_add(p + 2, len - 2, false);
      evbuffer_drain(input, len);

      svc->bytes += len;
      svc->packets++;
    }
}

/* The player is controlled from the worker thread: its commands wait for the
 * player, and starting or stopping can take seconds while it talks to the
 * speakers. The sender waits for our replies (Music.app even in its main
 * thread), so the receiver thread must never block on the player.
 */
enum rx_player_action
{
  RX_PLAYER_START,
  RX_PLAYER_FLUSH,
  RX_PLAYER_STOP,
  RX_PLAYER_VOLUME,
};

struct rx_player_job
{
  enum rx_player_action action;
  int session_id;
  int volume;
  char sender[128];
  char why[32];
};

// Thread: worker
static void
playback_autostart(struct rx_player_job *job)
{
  struct player_status status;
  struct media_file_info mfi = { 0 };
  struct db_queue_item qi;
  struct db_queue_item *queue_item;
  struct db_queue_add_info queue_add_info;
  const char *sender = job->sender;
  int ret;

  ret = player_get_status(&status);
  if (ret == 0 && rx_queue_item_id && status.item_id == rx_queue_item_id && status.status != PLAY_STOPPED)
    return; // Already ours

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: starting playback of the AirPlay stream from '%s'\n", job->session_id, sender);

  mfi.path = safe_asprintf("airplay://%s", rx_device_id_raop);
  mfi.virtual_path = safe_asprintf("/%s", mfi.path);
  mfi.fname = strdup(rx_name);
  mfi.title = safe_asprintf("AirPlay from %s", sender);
  mfi.artist = strdup(sender);
  mfi.album = strdup(rx_name);
  mfi.type = strdup("aac");
  mfi.codectype = strdup("mp4a");
  mfi.description = strdup("AirPlay 2 stream");
  mfi.data_kind = DATA_KIND_AIRPLAY;
  mfi.media_kind = MEDIA_KIND_MUSIC;
  mfi.time_modified = time(NULL);

  db_queue_item_from_mfi(&qi, &mfi);

  // Like pipe autostart, the stream replaces what was queued
  player_playback_stop();
  db_queue_clear(0);

  ret = db_queue_add_start(&queue_add_info, -1);
  if (ret == 0)
    {
      ret = db_queue_add_next(&queue_add_info, &qi);
      ret = db_queue_add_end(&queue_add_info, 0, 0, ret);
    }

  free_queue_item(&qi, 1);
  free_mfi(&mfi, 1);

  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not add the AirPlay stream to the queue\n", job->session_id);
      return;
    }

  queue_item = db_queue_fetch_byitemid(queue_add_info.new_item_id);
  if (!queue_item)
    return;

  ret = player_playback_start_byitem(queue_item);
  free_queue_item(queue_item, 0);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not start playback of the AirPlay stream\n", job->session_id);
      return;
    }

  rx_queue_item_id = queue_add_info.new_item_id;
}

// Drops what the player has buffered from the stream, so a pause, skip or
// seek is heard right away instead of after the buffered audio has played
// out. The player resumes by itself when the input has refilled its buffer
// (see playback_flush() in player.c). Only if it is our stream that plays.
// Thread: worker
static void
player_flush_if_ours(struct rx_player_job *job)
{
  struct player_status status;

  if (player_get_status(&status) < 0)
    return;
  if (!rx_queue_item_id || status.item_id != rx_queue_item_id || status.status != PLAY_PLAYING)
    return;

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: flushing the player (%s)\n", job->session_id, job->why);
  player_playback_flush();
}

// When the sender stops streaming to us, stop right away rather than play
// out what is buffered (the input would otherwise just signal end of file)
// Thread: worker
static void
player_stop_if_ours(struct rx_player_job *job)
{
  struct player_status status;

  if (player_get_status(&status) < 0)
    return;
  if (!rx_queue_item_id || status.item_id != rx_queue_item_id || status.status == PLAY_STOPPED)
    return;

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: stopping the player (%s)\n", job->session_id, job->why);
  player_playback_stop();
}

// Sender volume is in dB, -30 to 0, and -144 for mute (same mapping as the
// pipe input uses for Shairport's pvol)
static void player_job_queue(enum rx_player_action action, struct rx_session *s, const char *why, int volume);

static void
volume_apply(struct rx_session *s, float airplay_volume)
{
  int volume;

  if ((int)airplay_volume == -144)
    volume = 0;
  else if (airplay_volume >= -30.0 && airplay_volume <= 0.0)
    volume = (int)(100.0 + (airplay_volume / 30.0 * 100.0));
  else
    return;

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: sender volume %.2f dB, setting volume %d\n", s->id, airplay_volume, volume);
  player_job_queue(RX_PLAYER_VOLUME, s, "volume", volume);
}

// Thread: worker
static void
player_job_cb(void *arg)
{
  struct rx_player_job *job = arg;

  switch (job->action)
    {
      case RX_PLAYER_START:
	playback_autostart(job);
	break;
      case RX_PLAYER_FLUSH:
	player_flush_if_ours(job);
	break;
      case RX_PLAYER_STOP:
	player_stop_if_ours(job);
	break;
      case RX_PLAYER_VOLUME:
	player_volume_set(job->volume);
	break;
    }
}

// Thread: airplayrx
static void
player_job_queue(enum rx_player_action action, struct rx_session *s, const char *why, int volume)
{
  struct rx_player_job job = { .action = action, .session_id = s->id, .volume = volume };

  snprintf(job.sender, sizeof(job.sender), "%s", s->client_name ? s->client_name : s->peer_addr);
  snprintf(job.why, sizeof(job.why), "%s", why);

  worker_execute(player_job_cb, &job, sizeof(job), 0);
}


/* ------------------------------- Sessions ---------------------------------- */

static void
session_free(struct rx_session *s)
{
  struct rx_session *p;

  if (!s)
    return;

  if (rx_sessions == s)
    rx_sessions = s->next;
  else
    {
      for (p = rx_sessions; p && p->next != s; p = p->next)
	;
      if (p)
	p->next = s->next;
    }

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d from %s (%s) closed\n", s->id, s->peer_addr, s->client_name ? s->client_name : "unknown sender");

  stream_end(s->id, "sender disconnected");
  session_streams_stop(s);
  service_free(s->event_svc);

  if (s->bev)
    bufferevent_free(s->bev);
  if (s->plain_in)
    evbuffer_free(s->plain_in);
  if (s->setup_ctx)
    pair_setup_free(s->setup_ctx);
  if (s->verify_ctx)
    pair_verify_free(s->verify_ctx);
  if (s->cipher)
    pair_cipher_free(s->cipher);

  free(s->client_name);
  free(s->timing_protocol);
  free(s->group_uuid);
  free(s);
}

static void
session_cipher_set(struct rx_session *s, const uint8_t *secret, size_t secret_len)
{
  if (s->cipher)
    pair_cipher_free(s->cipher);

  memcpy(s->shared_secret, secret, secret_len);
  s->shared_secret_len = secret_len;

  // Not active yet: the reply to this request is still plaintext. Like the
  // sender, we switch on encryption from the next request (see read_cb).
  s->cipher = pair_cipher_new(PAIR_SERVER_HOMEKIT, PAIR_CHANNEL_CONTROL, secret, secret_len, NULL);
  if (!s->cipher)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not create control channel cipher\n", s->id);
}


/* ------------------------- Request and response ---------------------------- */

static void
request_clear(struct rx_request *req)
{
  keyval_clear(&req->headers);
  free(req->body);
  if (req->plist)
    plist_free(req->plist);
}

static void
response_clear(struct rx_response *resp)
{
  keyval_clear(&resp->headers);
  free(resp->body);
}

static void
response_body_set(struct rx_response *resp, uint8_t *body, size_t len, const char *content_type)
{
  free(resp->body);
  resp->body = body;
  resp->body_len = len;
  if (content_type)
    keyval_add(&resp->headers, "Content-Type", content_type);
}

static void
response_plist_set(struct rx_response *resp, plist_t plist)
{
  uint8_t *data = NULL;
  size_t len = 0;

  if (wplist_to_bin(&data, &len, plist) < 0)
    {
      resp->code = 500;
      return;
    }

  response_body_set(resp, data, len, "application/x-apple-binary-plist");
}

// Returns 1 if a complete request was parsed, 0 if more data is needed and -1
// if the input is not a valid request
static int
request_parse(struct rx_request *req, struct evbuffer *in)
{
  struct evbuffer_ptr eoh;
  char *head;
  char *line;
  char *next;
  char *colon;
  const char *clen_str;
  size_t head_len;
  long clen = 0;

  eoh = evbuffer_search(in, "\r\n\r\n", 4, NULL);
  if (eoh.pos < 0)
    return (evbuffer_get_length(in) > RX_HEADER_MAX) ? -1 : 0;

  head_len = eoh.pos + 4;
  CHECK_NULL(L_AIRPLAY_RX, head = malloc(head_len + 1));
  evbuffer_copyout(in, head, head_len);
  head[head_len] = '\0';

  line = head;
  next = strstr(line, "\r\n");
  *next = '\0';
  if (sscanf(line, "%31s %511s", req->method, req->path) != 2)
    {
      free(head);
      return -1;
    }

  for (line = next + 2; *line && strncmp(line, "\r\n", 2) != 0; line = next + 2)
    {
      next = strstr(line, "\r\n");
      if (!next)
	break;
      *next = '\0';

      colon = strchr(line, ':');
      if (!colon)
	continue;
      *colon = '\0';
      for (colon++; *colon == ' '; colon++)
	;
      keyval_add(&req->headers, line, colon);
    }
  free(head);

  clen_str = keyval_get(&req->headers, "Content-Length");
  if (clen_str)
    clen = strtol(clen_str, NULL, 10);
  if (clen < 0 || clen > RX_BODY_MAX)
    return -1;

  if (evbuffer_get_length(in) < head_len + (size_t)clen)
    {
      // Wait for the rest of the body, parse again then
      keyval_clear(&req->headers);
      return 0;
    }

  evbuffer_drain(in, head_len);
  if (clen > 0)
    {
      CHECK_NULL(L_AIRPLAY_RX, req->body = malloc(clen));
      evbuffer_remove(in, req->body, clen);
      req->body_len = clen;
    }

  return 1;
}

static const char *
status_text(int code)
{
  switch (code)
    {
      case 200: return "OK";
      case 400: return "Bad Request";
      case 404: return "Not Found";
      case 453: return "Not Enough Bandwidth";
      case 470: return "Connection Authorization Required";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      default: return "Error";
    }
}

static int
response_send(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  struct evbuffer *out;
  struct onekeyval *okv;
  const char *cseq;
  uint8_t *ciphertext = NULL;
  size_t ciphertext_len = 0;
  size_t len;
  ssize_t ret;

  CHECK_NULL(L_AIRPLAY_RX, out = evbuffer_new());

  evbuffer_add_printf(out, "RTSP/1.0 %d %s\r\n", resp->code, status_text(resp->code));
  cseq = keyval_get(&req->headers, "CSeq");
  if (cseq)
    evbuffer_add_printf(out, "CSeq: %s\r\n", cseq);
  evbuffer_add_printf(out, "Server: AirTunes/%s\r\n", RX_SRCVERS);
  for (okv = resp->headers.head; okv; okv = okv->next)
    evbuffer_add_printf(out, "%s: %s\r\n", okv->name, okv->value);
  evbuffer_add_printf(out, "Content-Length: %zu\r\n\r\n", resp->body_len);
  if (resp->body_len)
    evbuffer_add(out, resp->body, resp->body_len);

  if (!s->encrypted)
    {
      ret = bufferevent_write_buffer(s->bev, out);
      evbuffer_free(out);
      return ret;
    }

  len = evbuffer_get_length(out);
  ret = pair_encrypt(&ciphertext, &ciphertext_len, evbuffer_pullup(out, -1), len, s->cipher);
  evbuffer_free(out);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not encrypt response: %s\n", s->id, pair_cipher_errmsg(s->cipher));
      free(ciphertext);
      return -1;
    }

  ret = bufferevent_write(s->bev, ciphertext, ciphertext_len);
  free(ciphertext);
  return ret;
}


/* -------------------------------- Handlers --------------------------------- */

static void
handle_info(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  plist_t root;
  plist_t caps;
  plist_t formats;
  uint8_t *txt;
  size_t txt_len;
  const char *value;

  value = keyval_get(&req->headers, "X-Apple-Client-Name");
  if (value && !s->client_name)
    s->client_name = strdup(value);

  root = plist_new_dict();
  wplist_dict_add_bool(root, "canRecordScreenStream", false);
  wplist_dict_add_string(root, "deviceID", rx_device_id);
  wplist_dict_add_uint(root, "features", ((uint64_t)RX_FEATURES_HI << 32) | RX_FEATURES_LO);
  wplist_dict_add_string(root, "featuresEx", RX_FEATURES_EX);
  plist_dict_set_item(root, "initialVolume", plist_new_real(RX_INITIAL_VOLUME));
  wplist_dict_add_bool(root, "keepAliveSendStatsAsBody", false);
  wplist_dict_add_string(root, "model", RX_MODEL);
  wplist_dict_add_string(root, "name", rx_name);
  wplist_dict_add_string(root, "pi", rx_pi);
  wplist_dict_add_data(root, "pk", rx_pk, sizeof(rx_pk));

  caps = plist_new_dict();
  wplist_dict_add_bool(caps, "supportsFPSSecureStop", false);
  wplist_dict_add_bool(caps, "supportsInterstitials", false);
  wplist_dict_add_bool(caps, "supportsUIForAudioOnlyContent", false);
  plist_dict_set_item(root, "playbackCapabilities", caps);

  wplist_dict_add_string(root, "protocolVersion", "1.1");
  wplist_dict_add_string(root, "psi", rx_psi);
  wplist_dict_add_bool(root, "screenDemoMode", false);
  wplist_dict_add_string(root, "senderAddress", s->peer_addr);
  wplist_dict_add_uint(root, "statusFlags", RX_STATUS_FLAGS);

  formats = plist_new_dict();
  wplist_dict_add_uint(formats, "audioStream", 262144);
  wplist_dict_add_uint(formats, "bufferStream", 1649282383872);
  plist_dict_set_item(root, "supportedFormats", formats);

  txt = txt_wire_make(&txt_len);
  wplist_dict_add_data(root, "txtAirPlay", txt, txt_len);
  free(txt);

  wplist_dict_add_uint(root, "volumeControlType", 3);
  wplist_dict_add_uint(root, "vv", 0);

  response_plist_set(resp, root);
  plist_free(root);
}

static void
handle_pair_setup(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  struct pair_result *result;
  uint8_t *out = NULL;
  size_t out_len = 0;
  int ret;

  if (!s->setup_ctx)
    {
      // No PIN means transient pairing is allowed (the sender chooses)
      s->setup_ctx = pair_setup_new(PAIR_SERVER_HOMEKIT, NULL, NULL, NULL, rx_device_id);
      if (!s->setup_ctx)
	{
	  resp->code = 500;
	  return;
	}
    }

  ret = pair_setup(&out, &out_len, s->setup_ctx, req->body, req->body_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: pair-setup failed: %s\n", s->id, pair_setup_errmsg(s->setup_ctx));
      free(out);
      resp->code = 470;
      return;
    }

  response_body_set(resp, out, out_len, "application/octet-stream");

  ret = pair_setup_result(NULL, &result, s->setup_ctx);
  if (ret == 0 && result->shared_secret_len > 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: transient pairing with %s completed\n", s->id, s->peer_addr);
      session_cipher_set(s, result->shared_secret, result->shared_secret_len);
    }
}

static void
handle_pair_verify(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  struct pair_result *result;
  uint8_t *out = NULL;
  size_t out_len = 0;
  int ret;

  if (!s->verify_ctx)
    {
      // No callback: we don't keep a list of paired clients (yet)
      s->verify_ctx = pair_verify_new(PAIR_SERVER_HOMEKIT, NULL, NULL, NULL, rx_device_id);
      if (!s->verify_ctx)
	{
	  resp->code = 500;
	  return;
	}
    }

  ret = pair_verify(&out, &out_len, s->verify_ctx, req->body, req->body_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: pair-verify failed: %s\n", s->id, pair_verify_errmsg(s->verify_ctx));
      free(out);
      resp->code = 470;
      return;
    }

  response_body_set(resp, out, out_len, "application/octet-stream");

  ret = pair_verify_result(&result, s->verify_ctx);
  if (ret == 0 && result->shared_secret_len > 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: pair-verify with %s completed\n", s->id, s->peer_addr);
      session_cipher_set(s, result->shared_secret, result->shared_secret_len);
    }
}

static void
handle_fp_setup(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  const int version_pos = 4;
  const int type_pos = 5;
  const int seq_pos = 6;
  const int mode_pos = 14;
  const size_t setup2_suffix_len = 20;
  uint8_t *out = NULL;
  size_t len = 0;
  uint8_t mode;

  if (req->body_len < 16 || req->body[version_pos] != 3 || req->body[type_pos] != 1)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: unexpected fp-setup message (%zu bytes)\n", s->id, req->body_len);
      resp->code = 400;
      return;
    }

  if (req->body[seq_pos] == 1)
    {
      mode = req->body[mode_pos];
      len = sizeof(server_fp_reply1) - 1;
      if (mode == 0)
	out = memdup(server_fp_reply1, len);
      else if (mode == 1)
	out = memdup(server_fp_reply2, len);
      else if (mode == 2)
	out = memdup(server_fp_reply3, len);
      else if (mode == 3)
	out = memdup(server_fp_reply4, len);
    }
  else if (req->body[seq_pos] == 3 && req->body_len >= setup2_suffix_len)
    {
      len = sizeof(server_fp_header) - 1 + setup2_suffix_len;
      CHECK_NULL(L_AIRPLAY_RX, out = malloc(len));
      memcpy(out, server_fp_header, sizeof(server_fp_header) - 1);
      memcpy(out + sizeof(server_fp_header) - 1, req->body + req->body_len - setup2_suffix_len, setup2_suffix_len);
    }

  if (!out)
    {
      resp->code = 400;
      return;
    }

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: fp-setup step %d done\n", s->id, req->body[seq_pos]);
  response_body_set(resp, out, len, "application/octet-stream");
}

static plist_t
timing_peer_info_make(struct rx_session *s)
{
  plist_t info = plist_new_dict();
  plist_t addresses = plist_new_array();

  plist_array_append_item(addresses, plist_new_string(s->local_addr));
  plist_dict_set_item(info, "Addresses", addresses);
  wplist_dict_add_string(info, "ID", s->local_addr);

  return info;
}

static void
handle_setup_session(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  plist_t reply;
  char *str;
  plist_t item;
  bool remote_control_only = false;

  free(s->timing_protocol);
  plist_dict_get_string(req->plist, "timingProtocol", &s->timing_protocol);
  free(s->group_uuid);
  plist_dict_get_string(req->plist, "groupUUID", &s->group_uuid);
  if (plist_dict_get_string(req->plist, "name", &str))
    {
      free(s->client_name);
      s->client_name = str;
    }
  item = plist_dict_get_item(req->plist, "isRemoteControlOnly");
  if (item && plist_get_node_type(item) == PLIST_BOOLEAN)
    {
      uint8_t b = 0;
      plist_get_bool_val(item, &b);
      remote_control_only = b;
    }

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: session SETUP from '%s', timing %s, group %s%s\n", s->id,
	  s->client_name ? s->client_name : s->peer_addr, s->timing_protocol ? s->timing_protocol : "(none)",
	  s->group_uuid ? s->group_uuid : "(none)", remote_control_only ? ", remote control only" : "");

  if (s->timing_protocol && strcmp(s->timing_protocol, "PTP") != 0 && strcmp(s->timing_protocol, "None") != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: timing protocol '%s' not supported\n", s->id, s->timing_protocol);
      resp->code = 453;
      return;
    }

  if (!s->event_svc)
    s->event_svc = service_new(s, RX_SERVICE_EVENT, SOCK_STREAM, "AirPlay receiver events");
  if (!s->event_svc)
    {
      resp->code = 500;
      return;
    }

  reply = plist_new_dict();
  wplist_dict_add_uint(reply, "eventPort", s->event_svc->port);
  if (s->timing_protocol && strcmp(s->timing_protocol, "PTP") == 0)
    {
      wplist_dict_add_uint(reply, "timingPort", 0);
      plist_dict_set_item(reply, "timingPeerInfo", timing_peer_info_make(s));
    }

  response_plist_set(resp, reply);
  plist_free(reply);
}

static void
handle_setup_streams(struct rx_session *s, struct rx_request *req, struct rx_response *resp, plist_t streams)
{
  plist_t stream;
  plist_t reply;
  plist_t reply_streams;
  plist_t reply_stream;
  plist_t item;
  uint64_t type = 0;
  uint64_t ct = 0;
  uint64_t spf = 0;
  uint64_t sr = 0;
  uint64_t audio_format = 0;
  uint64_t shk_len = 0;
  char *shk = NULL;

  if (plist_get_node_type(streams) != PLIST_ARRAY || plist_array_get_size(streams) < 1)
    {
      resp->code = 400;
      return;
    }

  stream = plist_array_get_item(streams, 0);
  plist_dict_get_uint_val(stream, "type", &type);
  plist_dict_get_uint_val(stream, "ct", &ct);
  plist_dict_get_uint_val(stream, "spf", &spf);
  plist_dict_get_uint_val(stream, "sr", &sr);
  plist_dict_get_uint_val(stream, "audioFormat", &audio_format);
  item = plist_dict_get_item(stream, "shk");
  if (item && plist_get_node_type(item) == PLIST_DATA)
    plist_get_data_val(item, &shk, &shk_len);

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: stream SETUP type %" PRIu64 " (%s), ct %" PRIu64 ", spf %" PRIu64 ", sr %" PRIu64 ", audioFormat 0x%" PRIx64 ", session key %" PRIu64 " bytes\n",
	  s->id, type, type == 96 ? "realtime" : type == 103 ? "buffered" : type == 130 ? "remote control" : "unknown",
	  ct, spf, sr, audio_format, shk_len);

  session_streams_stop(s);
  s->stream_type = type;
  s->anchor_valid = false;

  if (type == 96 && audio_format && audio_format != RX_FMT_ALAC_44100_S16)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: realtime audioFormat 0x%" PRIx64 " is not ALAC 44100/16/2, playback will fail\n", s->id, audio_format);

  if (type == 96 || type == 103)
    stream_start(s, (uint8_t *)shk, shk_len);
  if (shk)
    {
      sodium_memzero(shk, shk_len);
      free(shk);
    }

  switch (type)
    {
      case 96:
	s->data_svc = service_new(s, RX_SERVICE_DATA, SOCK_DGRAM, "AirPlay receiver realtime audio");
	break;
      case 103:
      case 130:
	s->data_svc = service_new(s, RX_SERVICE_DATA, SOCK_STREAM, type == 103 ? "AirPlay receiver buffered audio" : "AirPlay receiver remote control");
	break;
      default:
	DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: unsupported stream type %" PRIu64 "\n", s->id, type);
	resp->code = 453;
	return;
    }

  if (type != 130)
    s->control_svc = service_new(s, RX_SERVICE_CONTROL, SOCK_DGRAM, "AirPlay receiver control");

  if (!s->data_svc || (type != 130 && !s->control_svc))
    {
      session_streams_stop(s);
      resp->code = 500;
      return;
    }

  reply_stream = plist_new_dict();
  if (type == 130)
    wplist_dict_add_uint(reply_stream, "streamID", 1);
  wplist_dict_add_uint(reply_stream, "type", type);
  wplist_dict_add_uint(reply_stream, "dataPort", s->data_svc->port);
  if (s->control_svc)
    wplist_dict_add_uint(reply_stream, "controlPort", s->control_svc->port);
  if (type == 103)
    wplist_dict_add_uint(reply_stream, "audioBufferSize", RX_AUDIO_BUFFER_SIZE);

  reply_streams = plist_new_array();
  plist_array_append_item(reply_streams, reply_stream);
  reply = plist_new_dict();
  plist_dict_set_item(reply, "streams", reply_streams);

  response_plist_set(resp, reply);
  plist_free(reply);
}

static void
handle_setup(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  plist_t streams;

  if (!req->plist)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: SETUP without a plist (AirPlay 1 sender?), not supported\n", s->id);
      resp->code = 501;
      return;
    }

  streams = plist_dict_get_item(req->plist, "streams");
  if (streams)
    handle_setup_streams(s, req, resp, streams);
  else
    handle_setup_session(s, req, resp);
}

static void
handle_teardown(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  if (req->plist && plist_dict_get_item(req->plist, "streams"))
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: TEARDOWN of the audio stream\n", s->id);
      stream_end(s->id, "TEARDOWN");
      player_job_queue(RX_PLAYER_STOP, s, "TEARDOWN", 0);
      session_streams_stop(s);
      return;
    }

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: TEARDOWN of the session\n", s->id);
  stream_end(s->id, "TEARDOWN");
  player_job_queue(RX_PLAYER_STOP, s, "TEARDOWN", 0);
  session_streams_stop(s);
  service_free(s->event_svc);
  s->event_svc = NULL;
}

static void
handle_get_parameter(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  char *body;

  if (req->body_len >= 6 && strncmp((char *)req->body, "volume", 6) == 0)
    {
      body = safe_asprintf("volume: %f\r\n", RX_INITIAL_VOLUME);
      response_body_set(resp, (uint8_t *)body, strlen(body), "text/parameters");
    }
}

static void
handle_set_parameter(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  const char *ctype = keyval_get(&req->headers, "Content-Type");
  char text[64];
  size_t len;

  if (ctype && strcmp(ctype, "text/parameters") == 0 && req->body_len > 0)
    {
      len = req->body_len < sizeof(text) - 1 ? req->body_len : sizeof(text) - 1;
      memcpy(text, req->body, len);
      text[len] = '\0';
      text[strcspn(text, "\r\n")] = '\0';
      DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: SET_PARAMETER %s\n", s->id, text);
      if (strncmp(text, "volume:", 7) == 0)
	volume_apply(s, strtof(text + 7, NULL));
    }
  else
    DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: SET_PARAMETER %s, %zu bytes\n", s->id, ctype ? ctype : "(no type)", req->body_len);
}

static uint64_t
now_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void
clock_log(int severity, struct rx_session *s, uint64_t clock_id)
{
  struct airptp_clock_info ci;

  if (ptpd_clock_info_get(&ci, clock_id) < 0)
    {
      DPRINTF(severity, L_AIRPLAY_RX, "Connection %d: not following the sender's clock %" PRIx64 " (yet)\n", s->id, clock_id);
      return;
    }

  DPRINTF(severity, L_AIRPLAY_RX, "Connection %d: sender clock %" PRIx64 " (%s): offset %" PRIi64 ".%06" PRIi64 " s, updated %" PRIu64 " ms ago, jitter %.1f us, drift %+.2f ppm, %" PRIu64 " samples, %" PRIu64 " steps\n",
	  s->id, ci.clock_id, ci.address, (int64_t)(ci.offset_ns / 1000000000LL), (int64_t)(llabs(ci.offset_ns % 1000000000LL) / 1000),
	  (now_ns() - ci.updated_ns) / 1000000, ci.jitter_us, ci.drift_ppm, ci.samples, ci.steps);
}

static void
handle_setrateanchortime(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  struct airptp_clock_info ci;
  uint64_t rate = 0;
  uint64_t rtptime = 0;
  uint64_t secs = 0;
  uint64_t frac = 0;
  uint64_t timeline_id = 0;
  uint64_t remote_ns;
  int64_t ahead_ms;

  if (!req->plist)
    {
      resp->code = 400;
      return;
    }

  plist_dict_get_uint_val(req->plist, "rate", &rate);
  plist_dict_get_uint_val(req->plist, "rtpTime", &rtptime);
  plist_dict_get_uint_val(req->plist, "networkTimeSecs", &secs);
  plist_dict_get_uint_val(req->plist, "networkTimeFrac", &frac);
  plist_dict_get_uint_val(req->plist, "networkTimeTimelineID", &timeline_id);

  s->anchor_rate = rate;
  if (!secs)
    {
      // Rate change only (pause), no new anchor
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: SETRATEANCHORTIME rate %" PRIu64 ", no anchor\n", s->id, rate);
      stream_anchor_set(rate, false, 0, 0);
      if (rate == 0)
	player_job_queue(RX_PLAYER_FLUSH, s, "pause", 0);
      return;
    }

  // networkTimeFrac is a 64 bit binary fraction of a second
  remote_ns = secs * 1000000000ULL + (((frac >> 32) * 1000000000ULL) >> 32);
  s->anchor_clock_id = timeline_id;

  if (ptpd_clock_info_get(&ci, timeline_id) < 0)
    {
      // Can't place the anchor on our clock, so play as soon as possible
      s->anchor_valid = false;
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: SETRATEANCHORTIME rate %" PRIu64 ", rtpTime %" PRIu64 " at %" PRIu64 ".%09" PRIu64 " on clock %" PRIx64 ", which we are not following, playing unsynchronised\n",
	      s->id, rate, rtptime, (uint64_t)(remote_ns / 1000000000ULL), (uint64_t)(remote_ns % 1000000000ULL), timeline_id);
      stream_anchor_set(rate, true, (uint32_t)rtptime, now_ns() + outputs_buffer_duration_ms_get() * 1000000ULL);
      if (rate)
	player_job_queue(RX_PLAYER_START, s, "anchor", 0);
      return;
    }

  s->anchor_valid = true;
  s->anchor_rtptime = (uint32_t)rtptime;
  s->anchor_local_ns = remote_ns - ci.offset_ns;
  ahead_ms = ((int64_t)s->anchor_local_ns - (int64_t)now_ns()) / 1000000;

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: SETRATEANCHORTIME rate %" PRIu64 ", rtpTime %" PRIu64 " plays %" PRIi64 " ms %s (sender clock %" PRIx64 ", jitter %.1f us, drift %+.2f ppm)\n",
	  s->id, rate, rtptime, (int64_t)llabs(ahead_ms), ahead_ms >= 0 ? "from now" : "ago", timeline_id, ci.jitter_us, ci.drift_ppm);

  stream_anchor_set(rate, true, s->anchor_rtptime, s->anchor_local_ns);
  if (rate)
    player_job_queue(RX_PLAYER_START, s, "anchor", 0);
}

static void
handle_flushbuffered(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  uint64_t from_seq = 0;
  uint64_t until_seq = 0;
  bool has_from;

  if (!req->plist || plist_dict_get_uint_val(req->plist, "flushUntilSeq", &until_seq) < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: FLUSHBUFFERED without flushUntilSeq, ignored\n", s->id);
      return;
    }

  has_from = (plist_dict_get_uint_val(req->plist, "flushFromSeq", &from_seq) == 0);

  // Senders count with more bits than the packets carry
  from_seq &= RX_SEQ_MASK;
  until_seq &= RX_SEQ_MASK;

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: FLUSHBUFFERED %s seq %" PRIu64 " until %" PRIu64 "\n", s->id,
	  has_from ? "from" : "everything before", has_from ? from_seq : until_seq, until_seq);

  stream_flush(has_from, (uint32_t)from_seq, (uint32_t)until_seq);

  // A range flush is for audio further ahead, what plays now stays
  if (!has_from)
    player_job_queue(RX_PLAYER_FLUSH, s, "flush", 0);
}

// Realtime streams flush with "RTP-Info: seq=...;rtptime=..."
static void
handle_flush(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  const char *info = keyval_get(&req->headers, "RTP-Info");
  const char *p;
  uint32_t rtptime;

  if (s->stream_type != 96)
    return;

  p = info ? strstr(info, "rtptime=") : NULL;
  if (!p)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: FLUSH without rtptime, ignored\n", s->id);
      return;
    }

  rtptime = (uint32_t)strtoul(p + strlen("rtptime="), NULL, 10);
  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: FLUSH realtime audio before rtptime %u\n", s->id, rtptime);

  stream_flush_rtptime(rtptime);
  player_job_queue(RX_PLAYER_FLUSH, s, "flush", 0);
}

// The realtime control channel. The sender sends a sync packet (type 215)
// about once a second: bytes 4-7 an RTP time, 8-15 the sender's network time
// in nanoseconds, 16-19 the RTP time being sent now, 20-27 the clock ID. The
// frame that plays at the network time is the first RTP time minus
// RX_REALTIME_ANCHOR_FUDGE, which makes the latency 2 s (Shairport Sync uses
// the same). Retransmitted audio (type 214) is ignored, we don't ask for any.
// Thread: airplayrx
static void
realtime_control_packet(struct rx_session *s, const uint8_t *p, size_t len)
{
  struct airptp_clock_info ci;
  uint32_t rtptime;
  uint32_t rtptime_now;
  uint64_t remote_ns;
  uint64_t clock_id;
  uint64_t local_ns;
  int64_t predicted_ns;
  int64_t moved_ns;
  bool synced;

  if (len < 28 || p[1] != 215)
    return;

  rtptime = be32_get(p + 4) - RX_REALTIME_ANCHOR_FUDGE;
  remote_ns = be64_get(p + 8);
  rtptime_now = be32_get(p + 16);
  clock_id = be64_get(p + 20);

  synced = (ptpd_clock_info_get(&ci, clock_id) == 0);
  if (synced)
    local_ns = remote_ns - ci.offset_ns;
  else if (!s->anchor_valid)
    local_ns = now_ns(); // Unsynchronised: the network time is about now
  else
    return;

  if (s->anchor_valid)
    {
      // Only move the timeline if the sender's clock moved it noticeably,
      // every resync restarts the input on the timeline
      predicted_ns = (int64_t)s->anchor_local_ns + (int64_t)(int32_t)(rtptime - s->anchor_rtptime) * 1000000000LL / 44100;
      moved_ns = (int64_t)local_ns - predicted_ns;
      if (llabs(moved_ns) < RX_REALTIME_RESYNC_NS)
	return;

      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: realtime timeline moved %+" PRIi64 " ms, resyncing\n", s->id, moved_ns / 1000000);
    }
  else
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: realtime anchor rtpTime %u plays %" PRIi64 " ms from now, sender latency %d frames, %s clock %" PRIx64 "\n",
	    s->id, rtptime, ((int64_t)local_ns - (int64_t)now_ns()) / 1000000, (int32_t)(rtptime_now - rtptime),
	    synced ? "following" : "NOT following, unsynchronised,", clock_id);

  s->anchor_valid = true;
  s->anchor_rate = 1;
  s->anchor_rtptime = rtptime;
  s->anchor_clock_id = clock_id;
  s->anchor_local_ns = local_ns;

  stream_anchor_set(1, true, rtptime, local_ns);
  player_job_queue(RX_PLAYER_START, s, "realtime anchor", 0);
}

static void
request_handle(struct rx_session *s, struct rx_request *req)
{
  struct rx_response resp = { .code = 200 };
  const char *ctype;
  char *str;

  ctype = keyval_get(&req->headers, "Content-Type");
  if (req->body_len && ctype && strcmp(ctype, "application/x-apple-binary-plist") == 0)
    plist_from_bin((char *)req->body, (uint32_t)req->body_len, &req->plist);

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: %s %s (%zu bytes%s)\n", s->id, req->method, req->path, req->body_len,
	  s->encrypted ? ", encrypted" : "");
  plist_log(E_DBG, req->method, req->plist);

  if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/info") == 0)
    handle_info(s, req, &resp);
  else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/pair-setup") == 0)
    handle_pair_setup(s, req, &resp);
  else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/pair-verify") == 0)
    handle_pair_verify(s, req, &resp);
  else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/fp-setup") == 0)
    handle_fp_setup(s, req, &resp);
  else if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/feedback") == 0)
    {
      s->feedback_count++;
      if (s->anchor_clock_id)
	clock_log((s->feedback_count % 5 == 0) ? E_LOG : E_DBG, s, s->anchor_clock_id);
    }
  else if (strcmp(req->method, "POST") == 0 && (strcmp(req->path, "/command") == 0 ||
	   strcmp(req->path, "/audioMode") == 0 || strcmp(req->path, "/configure") == 0))
    ; // Accept
  else if (strcmp(req->method, "OPTIONS") == 0)
    keyval_add(&resp.headers, "Public", "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, OPTIONS, POST, GET, PUT, GET_PARAMETER, SET_PARAMETER, SETPEERS, SETPEERSX, SETRATEANCHORTIME, SETRATE");
  else if (strcmp(req->method, "SETUP") == 0)
    handle_setup(s, req, &resp);
  else if (strcmp(req->method, "TEARDOWN") == 0)
    handle_teardown(s, req, &resp);
  else if (strcmp(req->method, "GET_PARAMETER") == 0)
    handle_get_parameter(s, req, &resp);
  else if (strcmp(req->method, "SET_PARAMETER") == 0)
    handle_set_parameter(s, req, &resp);
  else if (strcmp(req->method, "SETRATEANCHORTIME") == 0)
    handle_setrateanchortime(s, req, &resp);
  else if (strcmp(req->method, "RECORD") == 0)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: RECORD, sender '%s' starts streaming\n", s->id, s->client_name ? s->client_name : s->peer_addr);
  else if (strcmp(req->method, "SETPEERS") == 0 || strcmp(req->method, "SETPEERSX") == 0)
    {
      str = req->plist ? wplist_to_xml(req->plist) : NULL;
      DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: %s %s\n", s->id, req->method, str ? str : "");
      free(str);
    }
  else if (strcmp(req->method, "FLUSHBUFFERED") == 0)
    handle_flushbuffered(s, req, &resp);
  else if (strcmp(req->method, "FLUSH") == 0)
    handle_flush(s, req, &resp);
  else if (strcmp(req->method, "SETRATE") == 0 || strcmp(req->method, "PAUSE") == 0)
    ; // Accept
  else
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: unhandled request %s %s\n", s->id, req->method, req->path);
      resp.code = 501;
    }

  if (resp.code != 200)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: %s %s answered %d\n", s->id, req->method, req->path, resp.code);

  if (response_send(s, req, &resp) < 0)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not send response to %s %s\n", s->id, req->method, req->path);

  response_clear(&resp);
}

static void
session_read_cb(struct bufferevent *bev, void *arg)
{
  struct rx_session *s = arg;
  struct evbuffer *input = bufferevent_get_input(bev);
  struct rx_request req;
  uint8_t *plain = NULL;
  size_t plain_len = 0;
  ssize_t consumed;
  int ret;

  // Pairing completed with the previous request, so from now on the sender
  // encrypts
  if (s->cipher && !s->encrypted)
    {
      DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: control channel is now encrypted\n", s->id);
      s->encrypted = true;
    }

  if (s->encrypted)
    {
      consumed = pair_decrypt(&plain, &plain_len, evbuffer_pullup(input, -1), evbuffer_get_length(input), s->cipher);
      if (consumed < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: could not decrypt request: %s\n", s->id, pair_cipher_errmsg(s->cipher));
	  free(plain);
	  session_free(s);
	  return;
	}

      evbuffer_drain(input, consumed);
      if (plain_len > 0)
	evbuffer_add(s->plain_in, plain, plain_len);
      free(plain);
    }
  else
    evbuffer_add_buffer(s->plain_in, input);

  for (;;)
    {
      memset(&req, 0, sizeof(req));
      ret = request_parse(&req, s->plain_in);
      if (ret == 0)
	break;
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: invalid request from %s, closing\n", s->id, s->peer_addr);
	  request_clear(&req);
	  session_free(s);
	  return;
	}

      request_handle(s, &req);
      request_clear(&req);
    }
}

static void
session_event_cb(struct bufferevent *bev, short events, void *arg)
{
  struct rx_session *s = arg;

  if (events & BEV_EVENT_ERROR)
    DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: socket error: %s\n", s->id, evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));

  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
    session_free(s);
}

static void
rx_accept_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *address, int socklen, void *arg)
{
  struct rx_session *s;
  struct sockaddr_storage local;
  socklen_t local_len = sizeof(local);

  CHECK_NULL(L_AIRPLAY_RX, s = calloc(1, sizeof(struct rx_session)));
  CHECK_NULL(L_AIRPLAY_RX, s->plain_in = evbuffer_new());
  s->id = ++rx_session_counter;

  sockaddr_to_string(s->peer_addr, sizeof(s->peer_addr), address);
  if (getsockname(fd, (struct sockaddr *)&local, &local_len) == 0)
    sockaddr_to_string(s->local_addr, sizeof(s->local_addr), (struct sockaddr *)&local);

  s->bev = bufferevent_socket_new(evbase_rx, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!s->bev)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not create connection for %s\n", s->peer_addr);
      evbuffer_free(s->plain_in);
      free(s);
      close(fd);
      return;
    }

  bufferevent_setcb(s->bev, session_read_cb, NULL, session_event_cb, s);
  bufferevent_enable(s->bev, EV_READ);

  s->next = rx_sessions;
  rx_sessions = s;

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d from %s (to %s)\n", s->id, s->peer_addr, s->local_addr);
}

static void
rx_accept_error_cb(struct evconnlistener *listener, void *ctx)
{
  int err = EVUTIL_SOCKET_ERROR();
  DPRINTF(E_LOG, L_AIRPLAY_RX, "Error accepting connection: %d (%s)\n", err, evutil_socket_error_to_string(err));
}


/* --------------------------------- Thread ---------------------------------- */

static void *
airplay_receiver(void *arg)
{
  thread_setname("airplayrx");

  event_base_dispatch(evbase_rx);

  if (rx_enabled)
    DPRINTF(E_INFO, L_AIRPLAY_RX, "AirPlay receiver event loop terminated\n");

  pthread_exit(NULL);
}

static void
mdns_register_all(void)
{
  char **txt;
  int ret;

  txt = txt_airplay_make(false);
  ret = mdns_register(rx_name, "_airplay._tcp", rx_port, txt);
  txt_free(txt);
  if (ret < 0)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not announce '%s' as _airplay._tcp\n", rx_name);

  txt = txt_raop_make();
  ret = mdns_register(rx_raop_name, "_raop._tcp", rx_port, txt);
  txt_free(txt);
  if (ret < 0)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not announce '%s' as _raop._tcp\n", rx_raop_name);
}


/* ---------------------------------- API ------------------------------------ */

void
airplay_receiver_stats_json_add(struct json_object *obj)
{
  json_object *rx = json_object_new_object();
  struct airptp_clock_info ci;
  uint64_t clock_id = 0;
  int64_t anchor_in_ms = 0;
  bool anchored;

  json_object_object_add(rx, "enabled", json_object_new_boolean(rx_enabled));
  if (rx_enabled)
    {
      json_object_object_add(rx, "name", json_object_new_string(rx_name));
      json_object_object_add(rx, "device_id", json_object_new_string(rx_device_id));
    }

  pthread_mutex_lock(&rx_stream.lck);
  json_object_object_add(rx, "streaming", json_object_new_boolean(rx_stream.active));
  if (rx_stream.active)
    json_object_object_add(rx, "stream_type", json_object_new_string(rx_stream.realtime ? "realtime" : "buffered"));
  if (rx_stream.client_name)
    json_object_object_add(rx, "sender", json_object_new_string(rx_stream.client_name));
  json_object_object_add(rx, "playing", json_object_new_boolean(rx_stream.active && rx_stream.rate > 0));
  json_object_object_add(rx, "packets_in", json_object_new_int64(rx_stream.packets_in));
  json_object_object_add(rx, "packets_undecryptable", json_object_new_int64(rx_stream.packets_bad));
  json_object_object_add(rx, "packets_dropped", json_object_new_int64(rx_stream.packets_dropped));
  json_object_object_add(rx, "queued_bytes", json_object_new_int64(rx_stream.queued_bytes));
  json_object_object_add(rx, "late_ms_skipped", json_object_new_int64(rx_stream.late_ms_total));
  anchored = rx_stream.anchor_valid;
  if (anchored)
    anchor_in_ms = ((int64_t)rx_stream.anchor_local_ns - (int64_t)now_ns()) / 1000000;
  pthread_mutex_unlock(&rx_stream.lck);

  if (anchored)
    json_object_object_add(rx, "anchor_age_ms", json_object_new_int64(-anchor_in_ms));

  // The sender's clock, if we follow one
  if (ptpd_clock_info_get(&ci, clock_id) == 0)
    {
      json_object *clk = json_object_new_object();
      char id[24];

      snprintf(id, sizeof(id), "%" PRIx64, ci.clock_id);
      json_object_object_add(clk, "clock_id", json_object_new_string(id));
      json_object_object_add(clk, "address", json_object_new_string(ci.address));
      json_object_object_add(clk, "offset_ns", json_object_new_int64(ci.offset_ns));
      json_object_object_add(clk, "updated_ms_ago", json_object_new_int64((int64_t)(now_ns() - ci.updated_ns) / 1000000));
      json_object_object_add(clk, "jitter_us", json_object_new_double(ci.jitter_us));
      json_object_object_add(clk, "drift_ppm", json_object_new_double(ci.drift_ppm));
      json_object_object_add(clk, "samples", json_object_new_int64(ci.samples));
      json_object_object_add(clk, "steps", json_object_new_int64(ci.steps));
      json_object_object_add(rx, "sender_clock", clk);
    }

  json_object_object_add(obj, "receiver", rx);
}

bool
airplay_receiver_is_self(const char *name)
{
  return rx_enabled && name && strcmp(name, rx_name) == 0;
}

bool
airplay_receiver_is_idle(void)
{
  bool idle;

  pthread_mutex_lock(&rx_stream.lck);
  idle = !rx_stream.active || rx_stream.rate == 0;
  pthread_mutex_unlock(&rx_stream.lck);

  return idle;
}

int
airplay_receiver_init(void)
{
  cfg_t *rxcfg = cfg_getsec(cfg, "airplay_receiver");
  int ret;

  if (!rxcfg || !cfg_getbool(rxcfg, "enabled"))
    {
      DPRINTF(E_INFO, L_AIRPLAY_RX, "AirPlay receiver not enabled\n");
      return 0;
    }

  if (identity_init() < 0)
    return -1;

  rx_port = cfg_getint(rxcfg, "port");
  rx_socket.fd4 = -1;
  rx_socket.fd6 = -1;

  CHECK_NULL(L_AIRPLAY_RX, evbase_rx = event_base_new());
  CHECK_NULL(L_AIRPLAY_RX, cmdbase = commands_base_new(evbase_rx, NULL));

  ret = net_bind(&rx_socket, &rx_port, SOCK_STREAM, "AirPlay receiver");
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not bind the AirPlay receiver to port %hu\n", rx_port);
      goto bind_fail;
    }

  if (rx_socket.fd4 >= 0)
    {
      rx_listener4 = evconnlistener_new(evbase_rx, rx_accept_cb, NULL, 0, -1, rx_socket.fd4);
      if (!rx_listener4)
	goto listener_fail;
      evconnlistener_set_error_cb(rx_listener4, rx_accept_error_cb);
    }
  if (rx_socket.fd6 >= 0)
    {
      rx_listener6 = evconnlistener_new(evbase_rx, rx_accept_cb, NULL, 0, -1, rx_socket.fd6);
      if (!rx_listener6)
	goto listener_fail;
      evconnlistener_set_error_cb(rx_listener6, rx_accept_error_cb);
    }

  rx_enabled = true;

  ret = pthread_create(&tid_rx, NULL, airplay_receiver, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not spawn AirPlay receiver thread: %s\n", strerror(ret));
      rx_enabled = false;
      goto listener_fail;
    }

  mdns_register_all();

  DPRINTF(E_LOG, L_AIRPLAY_RX, "AirPlay 2 receiver '%s' listening on port %hu (device id %s)\n", rx_name, rx_port, rx_device_id);
  return 0;

 listener_fail:
  DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not create AirPlay receiver listener\n");
  if (rx_listener4)
    evconnlistener_free(rx_listener4);
  if (rx_listener6)
    evconnlistener_free(rx_listener6);
  rx_listener4 = NULL;
  rx_listener6 = NULL;
  net_socket_close(&rx_socket);
 bind_fail:
  commands_base_free(cmdbase);
  event_base_free(evbase_rx);
  evbase_rx = NULL;
  return -1;
}

void
airplay_receiver_deinit(void)
{
  int ret;

  if (!rx_enabled)
    return;

  // Stops the event loop
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_rx, NULL);
  if (ret != 0)
    DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not join AirPlay receiver thread: %s\n", strerror(ret));

  rx_enabled = false;

  while (rx_sessions)
    session_free(rx_sessions);

  if (rx_listener4)
    evconnlistener_free(rx_listener4);
  if (rx_listener6)
    evconnlistener_free(rx_listener6);
  net_socket_close(&rx_socket);

  event_base_free(evbase_rx);
  evbase_rx = NULL;
}


/* ---------------------------------- Input ---------------------------------- */
/*                             Thread: input                                    */

// How many packets (of 1024 samples) to decode per play() call
#define RX_INPUT_PACKETS_PER_READ 4
// Start writing when the first frame is due within this time
#define RX_INPUT_START_MARGIN_NS 10000000LL

struct rx_input_ctx
{
  AVCodecContext *dec;
  AVPacket *pkt;
  AVFrame *frame;
  uint32_t ssrc;

  uint32_t generation;
  bool synced;
  struct timespec sync_ts;
  short pending_flags;

  // Realtime: the RTP time the next packet should have, to fill lost ones
  bool has_next_rtptime;
  uint32_t next_rtptime;
  bool resync; // Gap too long to fill, find our place again

  int16_t *pcm;
  size_t pcm_samples;
};

static void
decoder_close(struct rx_input_ctx *ctx)
{
  avcodec_free_context(&ctx->dec);
  av_packet_free(&ctx->pkt);
  av_frame_free(&ctx->frame);
  ctx->ssrc = 0;
}

// Realtime audio is ALAC with the parameters every AirPlay sender uses (the
// AirPlay 1 fmtp "96 352 0 16 40 10 14 2 255 0 0 44100"). libavcodec wants
// them as the 36 byte ALAC magic cookie.
static int
decoder_open_alac(struct rx_input_ctx *ctx, struct input_source *source)
{
  static const uint8_t cookie[36] = {
    0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c', 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, // Frame length 352
    0x00,                   // Compatible version
    16,                     // Bit depth
    40, 10, 14,             // Rice history mult, initial history, limit
    2,                      // Channels
    0x00, 0xff,             // Max run
    0x00, 0x00, 0x00, 0x00, // Max frame bytes
    0x00, 0x00, 0x00, 0x00, // Average bit rate
    0x00, 0x00, 0xac, 0x44, // Sample rate 44100
  };
  const AVCodec *codec;
  int ret;

  codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
  if (!codec)
    return -1;

  CHECK_NULL(L_AIRPLAY_RX, ctx->dec = avcodec_alloc_context3(codec));
  ctx->dec->sample_rate = 44100;
  av_channel_layout_default(&ctx->dec->ch_layout, 2);
  CHECK_NULL(L_AIRPLAY_RX, ctx->dec->extradata = av_mallocz(sizeof(cookie) + AV_INPUT_BUFFER_PADDING_SIZE));
  memcpy(ctx->dec->extradata, cookie, sizeof(cookie));
  ctx->dec->extradata_size = sizeof(cookie);

  ret = avcodec_open2(ctx->dec, codec, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not open ALAC decoder: %s\n", av_err2str(ret));
      decoder_close(ctx);
      return -1;
    }

  CHECK_NULL(L_AIRPLAY_RX, ctx->pkt = av_packet_alloc());
  CHECK_NULL(L_AIRPLAY_RX, ctx->frame = av_frame_alloc());
  ctx->ssrc = RX_FMT_ALAC_44100_S16;

  source->quality.sample_rate = 44100;
  source->quality.bits_per_sample = 16;
  source->quality.channels = 2;

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Decoding ALAC 44100 Hz stereo (realtime)\n");
  return 0;
}

static int
decoder_open(struct rx_input_ctx *ctx, struct input_source *source, uint32_t ssrc)
{
  const AVCodec *codec;
  uint8_t asc[2];
  int rate;
  int idx;
  int ret;

  decoder_close(ctx);

  if (ssrc == RX_FMT_ALAC_44100_S16)
    return decoder_open_alac(ctx, source);

  if (ssrc != RX_SSRC_AAC_44100 && ssrc != RX_SSRC_AAC_48000)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Audio format 0x%08x not supported yet\n", ssrc);
      return -1;
    }

  rate = ssrc_sample_rate(ssrc);
  idx = (rate == 48000) ? 3 : 4;

  codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  if (!codec)
    return -1;

  CHECK_NULL(L_AIRPLAY_RX, ctx->dec = avcodec_alloc_context3(codec));
  ctx->dec->sample_rate = rate;
  av_channel_layout_default(&ctx->dec->ch_layout, 2);

  // The packets are raw AAC-LC frames, so the decoder needs the
  // AudioSpecificConfig: object type 2 (LC), frequency index, 2 channels
  asc[0] = (2 << 3) | (idx >> 1);
  asc[1] = ((idx & 1) << 7) | (2 << 3);
  CHECK_NULL(L_AIRPLAY_RX, ctx->dec->extradata = av_mallocz(sizeof(asc) + AV_INPUT_BUFFER_PADDING_SIZE));
  memcpy(ctx->dec->extradata, asc, sizeof(asc));
  ctx->dec->extradata_size = sizeof(asc);

  ret = avcodec_open2(ctx->dec, codec, NULL);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Could not open AAC decoder: %s\n", av_err2str(ret));
      decoder_close(ctx);
      return -1;
    }

  CHECK_NULL(L_AIRPLAY_RX, ctx->pkt = av_packet_alloc());
  CHECK_NULL(L_AIRPLAY_RX, ctx->frame = av_frame_alloc());
  ctx->ssrc = ssrc;

  source->quality.sample_rate = rate;
  source->quality.bits_per_sample = 16;
  source->quality.channels = 2;

  DPRINTF(E_INFO, L_AIRPLAY_RX, "Decoding AAC-LC %d Hz stereo\n", rate);
  return 0;
}

static inline int16_t
float_to_s16(float f)
{
  if (f >= 1.0f)
    return INT16_MAX;
  if (f <= -1.0f)
    return INT16_MIN;
  return (int16_t)(f * 32767.0f);
}

// Converts a decoded frame to interleaved 16 bit stereo
static void
frame_write(struct evbuffer *evbuf, struct rx_input_ctx *ctx, AVFrame *frame)
{
  int nch = frame->ch_layout.nb_channels;
  int n = frame->nb_samples;
  int i;
  int c;
  int src;

  if (nch < 1 || n < 1)
    return;

  if ((size_t)n > ctx->pcm_samples)
    {
      free(ctx->pcm);
      CHECK_NULL(L_AIRPLAY_RX, ctx->pcm = malloc(n * 2 * sizeof(int16_t)));
      ctx->pcm_samples = n;
    }

  for (i = 0; i < n; i++)
    {
      for (c = 0; c < 2; c++)
	{
	  src = (c < nch) ? c : 0;
	  switch (frame->format)
	    {
	      case AV_SAMPLE_FMT_FLTP:
		ctx->pcm[2 * i + c] = float_to_s16(((float *)frame->data[src])[i]);
		break;
	      case AV_SAMPLE_FMT_FLT:
		ctx->pcm[2 * i + c] = float_to_s16(((float *)frame->data[0])[i * nch + src]);
		break;
	      case AV_SAMPLE_FMT_S16P:
		ctx->pcm[2 * i + c] = ((int16_t *)frame->data[src])[i];
		break;
	      case AV_SAMPLE_FMT_S16:
		ctx->pcm[2 * i + c] = ((int16_t *)frame->data[0])[i * nch + src];
		break;
	      default:
		ctx->pcm[2 * i + c] = 0;
	    }
	}
    }

  evbuffer_add(evbuf, ctx->pcm, n * 2 * sizeof(int16_t));
}

// Realtime packets lost on the way would pull the rest of the stream earlier,
// so a short gap becomes silence and a long one a resync. Returns false if
// the packet is older than what was already written.
static bool
realtime_gap_fill(struct rx_input_ctx *ctx, struct input_source *source, struct rx_packet *pkt)
{
  static const int16_t silence[2 * RX_ALAC_FRAME_SAMPLES];
  int32_t gap;
  int32_t n;

  if (!ctx->has_next_rtptime)
    goto done;

  gap = (int32_t)(pkt->rtptime - ctx->next_rtptime);
  if (gap < 0)
    return false;

  if (gap > RX_REALTIME_GAP_MAX_FRAMES)
    {
      DPRINTF(E_LOG, L_AIRPLAY_RX, "Realtime audio jumped %d ms ahead, resyncing to the sender's timeline\n", gap * 1000 / 44100);
      ctx->resync = true;
      goto done;
    }

  if (gap > 0)
    DPRINTF(E_DBG, L_AIRPLAY_RX, "Filling %d ms of lost realtime audio with silence\n", gap * 1000 / 44100);

  for (; gap > 0; gap -= n)
    {
      n = (gap < RX_ALAC_FRAME_SAMPLES) ? gap : RX_ALAC_FRAME_SAMPLES;
      evbuffer_add(source->evbuf, silence, n * 2 * sizeof(int16_t));
    }

 done:
  ctx->has_next_rtptime = true;
  ctx->next_rtptime = pkt->rtptime + RX_ALAC_FRAME_SAMPLES;
  return true;
}

static void
packet_decode(struct rx_input_ctx *ctx, struct input_source *source, struct rx_packet *pkt)
{
  int ret;

  if (pkt->ssrc == RX_FMT_ALAC_44100_S16 && !realtime_gap_fill(ctx, source, pkt))
    return;

  if (pkt->ssrc != ctx->ssrc && decoder_open(ctx, source, pkt->ssrc) < 0)
    return;

  av_packet_unref(ctx->pkt);
  if (av_new_packet(ctx->pkt, pkt->len) < 0)
    return;
  memcpy(ctx->pkt->data, pkt->data, pkt->len);

  ret = avcodec_send_packet(ctx->dec, ctx->pkt);
  if (ret < 0)
    {
      DPRINTF(E_WARN, L_AIRPLAY_RX, "Decoder rejected packet seq %u: %s\n", pkt->seq, av_err2str(ret));
      return;
    }

  while (avcodec_receive_frame(ctx->dec, ctx->frame) == 0)
    {
      frame_write(source->evbuf, ctx, ctx->frame);
      av_frame_unref(ctx->frame);
    }
}

static int
rxin_setup(struct input_source *source)
{
  struct rx_input_ctx *ctx;

  CHECK_NULL(L_AIRPLAY_RX, ctx = calloc(1, sizeof(struct rx_input_ctx)));
  CHECK_NULL(L_AIRPLAY_RX, source->evbuf = evbuffer_new());

  source->quality.sample_rate = 44100;
  source->quality.bits_per_sample = 16;
  source->quality.channels = 2;
  source->input_ctx = ctx;

  pthread_mutex_lock(&rx_stream.lck);
  ctx->generation = rx_stream.generation;
  pthread_mutex_unlock(&rx_stream.lck);

  return 0;
}

static int
rxin_stop(struct input_source *source)
{
  struct rx_input_ctx *ctx = source->input_ctx;

  if (ctx)
    {
      decoder_close(ctx);
      free(ctx->pcm);
      free(ctx);
    }
  if (source->evbuf)
    evbuffer_free(source->evbuf);

  source->input_ctx = NULL;
  source->evbuf = NULL;
  return 0;
}

static int
rxin_play(struct input_source *source)
{
  struct rx_input_ctx *ctx = source->input_ctx;
  struct rx_packet *batch = NULL;
  struct rx_packet *pkt;
  struct rx_packet *last;
  short flags = ctx->pending_flags;
  int64_t frame_offset;
  int64_t frame_ns;
  uint64_t late_frames;
  int64_t audible_ns;
  int64_t pts_ns;
  int64_t now;
  int max_packets;
  int n;
  int ret;

  pthread_mutex_lock(&rx_stream.lck);

  if (!rx_stream.active)
    {
      pthread_mutex_unlock(&rx_stream.lck);
      DPRINTF(E_INFO, L_AIRPLAY_RX, "AirPlay stream is gone, ending playback\n");
      input_write(NULL, NULL, INPUT_FLAG_EOF);
      return -1;
    }

  if (ctx->generation != rx_stream.generation || ctx->resync)
    {
      ctx->generation = rx_stream.generation;
      ctx->synced = false;
      ctx->resync = false;
      ctx->has_next_rtptime = false;
      rx_stream.late_frames_dropped = 0;
    }

  // Paused or waiting for the sender's anchor
  if (!rx_stream.anchor_valid || rx_stream.rate == 0)
    goto wait;

  if (!ctx->synced)
    {
      // Skip what should already have played before the anchor
      while (rx_stream.head && (int32_t)(rx_stream.head->rtptime + ssrc_frame_samples(rx_stream.head->ssrc) - rx_stream.anchor_rtptime) <= 0)
	{
	  stream_drop_head_locked();
	  rx_stream.packets_dropped++;
	}
      if (!rx_stream.head)
	goto wait;

      // When the first frame must be audible, and so when it must leave the
      // player, given the outputs' buffer. If that has already passed, drop
      // whole frames until we are back on the sender's timeline, so the audio
      // stays in step with the sender's video and its other speakers, instead
      // of playing late for the rest of the stream.
      now = (int64_t)now_ns();
      for (late_frames = 0; ; late_frames++)
	{
	  frame_offset = (int32_t)(rx_stream.head->rtptime - rx_stream.anchor_rtptime);
	  audible_ns = (int64_t)rx_stream.anchor_local_ns + frame_offset * 1000000000LL / ssrc_sample_rate(rx_stream.head->ssrc);
	  pts_ns = audible_ns - (int64_t)outputs_buffer_duration_ms_get() * 1000000LL;
	  frame_ns = ssrc_frame_samples(rx_stream.head->ssrc) * 1000000000LL / ssrc_sample_rate(rx_stream.head->ssrc);

	  if (pts_ns + frame_ns / 2 >= now)
	    break;

	  stream_drop_head_locked();
	  rx_stream.packets_dropped++;
	  if (!rx_stream.head)
	    {
	      rx_stream.late_frames_dropped += late_frames + 1;
	      goto wait;
	    }
	}
      rx_stream.late_frames_dropped += late_frames;

      if (pts_ns > now + RX_INPUT_START_MARGIN_NS)
	goto wait;

      if (rx_stream.late_frames_dropped)
	DPRINTF(E_LOG, L_AIRPLAY_RX, "Starting AirPlay stream on the sender's timeline, skipped %" PRIu64 " ms that was due before the outputs could play it\n",
		rx_stream.late_frames_dropped * ssrc_frame_samples(rx_stream.head->ssrc) * 1000 / ssrc_sample_rate(rx_stream.head->ssrc));
      else
	DPRINTF(E_LOG, L_AIRPLAY_RX, "Starting AirPlay stream at the sender's anchor (seq %u, rtptime %u)\n", rx_stream.head->seq, rx_stream.head->rtptime);
      rx_stream.late_ms_total += rx_stream.late_frames_dropped * ssrc_frame_samples(rx_stream.head->ssrc) * 1000 / ssrc_sample_rate(rx_stream.head->ssrc);
      rx_stream.late_frames_dropped = 0;

      // Less than half a frame late at most, the player can't start in the past
      if (pts_ns < now)
	pts_ns = now;

      ctx->sync_ts.tv_sec = pts_ns / 1000000000LL;
      ctx->sync_ts.tv_nsec = pts_ns % 1000000000LL;
      ctx->synced = true;
      flags |= INPUT_FLAG_SYNC | INPUT_FLAG_METADATA;
    }

  // Take a few packets off the queue, decode them without holding the lock.
  // Realtime audio arrives about 2 s ahead, so some can stay queued for
  // packets that arrive out of order.
  max_packets = RX_INPUT_PACKETS_PER_READ * RX_AAC_FRAME_SAMPLES / ssrc_frame_samples(rx_stream.head ? rx_stream.head->ssrc : 0);
  for (n = 0, last = NULL; rx_stream.head && n < max_packets; n++)
    {
      if (rx_stream.realtime && (int32_t)(rx_stream.tail->rtptime - rx_stream.head->rtptime) < RX_REALTIME_REORDER_FRAMES)
	break;

      pkt = rx_stream.head;
      rx_stream.head = pkt->next;
      if (!rx_stream.head)
	rx_stream.tail = NULL;
      rx_stream.queued_bytes -= pkt->len;
      pkt->next = NULL;
      if (last)
	last->next = pkt;
      else
	batch = pkt;
      last = pkt;
    }

  pthread_mutex_unlock(&rx_stream.lck);

  if (!batch)
    {
      input_wait();
      return 0;
    }

  while (batch)
    {
      pkt = batch;
      batch = pkt->next;
      packet_decode(ctx, source, pkt);
      packet_free(pkt);
    }

  ret = input_write(source->evbuf, &source->quality, flags);
  ctx->pending_flags = (ret == EAGAIN) ? flags : 0;
  return 0;

 wait:
  pthread_mutex_unlock(&rx_stream.lck);
  input_wait();
  return 0;
}

static int
rxin_ts_get(struct timespec *ts, struct input_source *source)
{
  struct rx_input_ctx *ctx = source->input_ctx;

  if (!ctx || !ctx->synced)
    return -1;

  *ts = ctx->sync_ts;
  return 0;
}

static int
rxin_metadata_get(struct input_metadata *metadata, struct input_source *source)
{
  pthread_mutex_lock(&rx_stream.lck);
  metadata->title = safe_asprintf("AirPlay from %s", rx_stream.client_name ? rx_stream.client_name : "a sender");
  pthread_mutex_unlock(&rx_stream.lck);

  return 0;
}

struct input_definition input_airplay =
{
  .name = "AirPlay receiver",
  .type = INPUT_TYPE_AIRPLAY,
  .disabled = 0,
  .setup = rxin_setup,
  .play = rxin_play,
  .stop = rxin_stop,
  .ts_get = rxin_ts_get,
  .metadata_get = rxin_metadata_get,
};
