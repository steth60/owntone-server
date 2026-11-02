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

#include "outputs/plist_wrap.h"
#include "pair_ap/pair.h"

#include "airplay_receiver.h"
#include "conffile.h"
#include "logger.h"
#include "misc.h"
#include "mdns.h"
#include "commands.h"

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
  free(shk); // Kept for phase 3 (audio decryption), not needed yet

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: stream SETUP type %" PRIu64 " (%s), ct %" PRIu64 ", spf %" PRIu64 ", sr %" PRIu64 ", audioFormat 0x%" PRIx64 ", session key %" PRIu64 " bytes\n",
	  s->id, type, type == 96 ? "realtime" : type == 103 ? "buffered" : type == 130 ? "remote control" : "unknown",
	  ct, spf, sr, audio_format, shk_len);

  session_streams_stop(s);
  s->stream_type = type;

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
      session_streams_stop(s);
      return;
    }

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: TEARDOWN of the session\n", s->id);
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
    }
  else
    DPRINTF(E_INFO, L_AIRPLAY_RX, "Connection %d: SET_PARAMETER %s, %zu bytes\n", s->id, ctype ? ctype : "(no type)", req->body_len);
}

static void
handle_setrateanchortime(struct rx_session *s, struct rx_request *req, struct rx_response *resp)
{
  uint64_t rate = 0;
  uint64_t rtptime = 0;
  uint64_t secs = 0;

  if (req->plist)
    {
      plist_dict_get_uint_val(req->plist, "rate", &rate);
      plist_dict_get_uint_val(req->plist, "rtpTime", &rtptime);
      plist_dict_get_uint_val(req->plist, "networkTimeSecs", &secs);
    }

  DPRINTF(E_LOG, L_AIRPLAY_RX, "Connection %d: SETRATEANCHORTIME rate %" PRIu64 ", rtpTime %" PRIu64 ", network time %" PRIu64 " s\n",
	  s->id, rate, rtptime, secs);
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
  else if (strcmp(req->method, "POST") == 0 && (strcmp(req->path, "/feedback") == 0 || strcmp(req->path, "/command") == 0 ||
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
  else if (strcmp(req->method, "FLUSHBUFFERED") == 0 || strcmp(req->method, "FLUSH") == 0 || strcmp(req->method, "SETRATE") == 0 ||
	   strcmp(req->method, "PAUSE") == 0)
    ; // Accept, playback control comes in phase 3
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

bool
airplay_receiver_is_self(const char *name)
{
  return rx_enabled && name && strcmp(name, rx_name) == 0;
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
