#ifndef __PTPD_H__
#define __PTPD_H__

uint64_t
ptpd_clock_id_get(void);

int
ptpd_slave_add(uint32_t *slave_id, const char *addr);

struct airptp_clock_info;

// Clock following: estimate for a remote master clock (e.g. an AirPlay sender
// when we are the receiver), see airptp_clock_info_get(). clock_id 0 gives the
// most recently updated clock.
int
ptpd_clock_info_get(struct airptp_clock_info *info, uint64_t clock_id);

void
ptpd_slave_remove(uint32_t slave_id);

// Looks for a shared airptpd daemon. If not found, binds priviliged ports 319
// and 320, so must be called before the server drops priviliges.
int
ptpd_find_or_bind(void);

int
ptpd_init(uint64_t clock_id_seed);

void
ptpd_deinit(void);

#endif /* !__PTPD_H__ */
