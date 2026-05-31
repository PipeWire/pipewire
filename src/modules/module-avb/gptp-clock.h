/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-License-Identifier: MIT */

/* gPTP time read from the NIC PHC (dynamic POSIX clock) mapped onto CLOCK_MONOTONIC_RAW,
 * decoupled from the system wall clock so it stays free for NTP. */

#ifndef AVB_GPTP_CLOCK_H
#define AVB_GPTP_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#include <spa/utils/defs.h>

#define AVB_CLOCKFD			3
#define AVB_FD_TO_CLOCKID(fd)		((~(clockid_t)(fd) << 3) | AVB_CLOCKFD)
#define AVB_GPTP_REFRESH_NS		(10 * SPA_NSEC_PER_MSEC)	/* re-anchor phase/freq ~100 Hz */
#define AVB_GPTP_READ_BRACKET_NS	(50 * SPA_NSEC_PER_USEC)	/* reject a jittered PHC read */

struct avb_gptp_clock {
	int phc_fd;
	clockid_t phc_id;
	bool ok;
	uint64_t base_mono;		/* CLOCK_MONOTONIC_RAW ns at last anchor */
	uint64_t base_gptp;		/* PHC ns at last anchor */
	double ratio;			/* d(phc)/d(mono) ~ 1.0 (the frequency offset) */
	uint64_t last_refresh_mono;
};

/* Resolve ifname -> PHC index via ETHTOOL_GET_TS_INFO, open /dev/ptpN. >=0 = phc_index. */
static inline int avb_gptp_clock_open(struct avb_gptp_clock *c, const char *ifname)
{
	struct ethtool_ts_info tsi;
	struct ifreq ifr;
	char path[32];
	int sock;

	memset(c, 0, sizeof(*c));
	c->phc_fd = -1;
	c->ratio = 1.0;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		return -1;
	}
	memset(&tsi, 0, sizeof(tsi));
	tsi.cmd = ETHTOOL_GET_TS_INFO;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_data = (void *)&tsi;
	if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
		close(sock);
		return -1;
	}
	close(sock);
	if (tsi.phc_index < 0) {
		return -1;
	}
	snprintf(path, sizeof(path), "/dev/ptp%d", tsi.phc_index);
	c->phc_fd = open(path, O_RDONLY);
	if (c->phc_fd < 0) {
		return -1;
	}
	c->phc_id = AVB_FD_TO_CLOCKID(c->phc_fd);
	c->ok = true;
	return tsi.phc_index;
}

/* Re-anchor (mono,gptp) and update the frequency ratio. Off the hot loop (~100 Hz). */
static inline void avb_gptp_clock_refresh(struct avb_gptp_clock *c)
{
	struct timespec m1, p, m2;
	uint64_t mono, gptp;
	double r;

	if (!c->ok) {
		return;
	}
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &m1) < 0 ||
	    clock_gettime(c->phc_id, &p) < 0 ||
	    clock_gettime(CLOCK_MONOTONIC_RAW, &m2) < 0) {
		return;
	}
	if (SPA_TIMESPEC_TO_NSEC(&m2) - SPA_TIMESPEC_TO_NSEC(&m1) > AVB_GPTP_READ_BRACKET_NS) {
		return;
	}
	mono = (SPA_TIMESPEC_TO_NSEC(&m1) + SPA_TIMESPEC_TO_NSEC(&m2)) / 2;
	gptp = SPA_TIMESPEC_TO_NSEC(&p);
	if (c->base_mono != 0 && mono > c->base_mono) {
		r = (double)(gptp - c->base_gptp) / (double)(mono - c->base_mono);
		if (r > 0.999 && r < 1.001) {
			c->ratio += 0.10 * (r - c->ratio);
		}
	}
	c->base_mono = mono;
	c->base_gptp = gptp;
	c->last_refresh_mono = mono;
}

/* gPTP time now, in ns; cheap monotonic read + phase/freq map. 0 if no PHC (caller falls back). */
static inline uint64_t avb_gptp_now(struct avb_gptp_clock *c)
{
	struct timespec ts;
	uint64_t mono;

	if (!c->ok) {
		return 0;
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	mono = SPA_TIMESPEC_TO_NSEC(&ts);
	if (c->base_mono == 0 || mono - c->last_refresh_mono > AVB_GPTP_REFRESH_NS) {
		avb_gptp_clock_refresh(c);
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
		mono = SPA_TIMESPEC_TO_NSEC(&ts);
	}
	if (c->base_mono == 0) {
		return 0;
	}
	return c->base_gptp + (uint64_t)((double)(mono - c->base_mono) * c->ratio);
}

#endif /* AVB_GPTP_CLOCK_H */
