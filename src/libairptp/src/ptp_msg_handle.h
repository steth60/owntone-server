#ifndef __PTP_MSG_HANDLE_H__
#define __PTP_MSG_HANDLE_H__

void
ptp_msg_announce_send(struct airptp_daemon *daemon);

void
ptp_msg_signaling_send(struct airptp_daemon *daemon);

void
ptp_msg_sync_send(struct airptp_daemon *daemon);

int
ptp_msg_peer_add_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port);

int
ptp_msg_peer_del_send(struct airptp_peer *peer, struct airptp_handle *hdl, unsigned short port);

void
ptp_msg_handle(struct airptp_daemon *daemon, uint8_t *msg, size_t msg_len, union utils_net_sockaddr *peer_addr, socklen_t peer_addrlen, uint64_t rx_ns);

// Caller must hold daemon->clock_sources_lck
void
ptp_msg_clock_info_fill(struct airptp_clock_info *info, struct airptp_clock_source *src);

int
ptp_msg_handle_init(void);

#endif // __PTP_MSG_HANDLE_H__
