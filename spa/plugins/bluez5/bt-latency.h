/* Spa Bluez5 ISO I/O */
/* SPDX-FileCopyrightText: Copyright © 2024 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_BT_LATENCY_H
#define SPA_BLUEZ5_BT_LATENCY_H

#include <time.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <linux/sockios.h>

#include <spa/utils/defs.h>
#include <spa/support/log.h>

#include "rate-control.h"

/* New kernel API */
#ifndef BT_SCM_ERROR
#define BT_SCM_ERROR 0x04
#endif
#ifndef BT_POLL_ERRQUEUE
#define BT_POLL_ERRQUEUE 21
#endif

/**
 * Bluetooth latency tracking.
 */
struct spa_bt_latency
{
	uint64_t value;
	struct spa_bt_ptp ptp;
	bool valid;
	bool disabled;

	struct {
		int64_t send[64];
		uint32_t pos;
		int64_t prev_tx;
	} impl;
};

static inline void spa_bt_latency_init(struct spa_bt_latency *lat, int fd,
		uint32_t period, struct spa_log *log)
{
	int so_timestamping = (SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
					SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY);
	uint32_t flag;
	int res;

	spa_zero(*lat);

	flag = 0;
	res = setsockopt(fd, SOL_BLUETOOTH, BT_POLL_ERRQUEUE, &flag, sizeof(flag));
	if (res < 0) {
		spa_log_warn(log, "setsockopt(BT_POLL_ERRQUEUE) failed (kernel feature not enabled?): %d (%m)", errno);
		lat->disabled = true;
		return;
	}

	res = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	if (res < 0) {
		spa_log_warn(log, "setsockopt(SO_TIMESTAMPING) failed (kernel feature not enabled?): %d (%m)", errno);
		lat->disabled = true;
		return;
	}

	/* Flush errqueue on start */
	do {
		res = recv(fd, NULL, 0, MSG_ERRQUEUE | MSG_DONTWAIT | MSG_TRUNC);
	} while (res == 0);

	spa_bt_ptp_init(&lat->ptp, period, period / 2);
}

static inline void spa_bt_latency_reset(struct spa_bt_latency *lat)
{
	lat->value = 0;
	lat->valid = false;
	spa_bt_ptp_init(&lat->ptp, lat->ptp.period, lat->ptp.period / 2);
}

static inline void spa_bt_latency_sent(struct spa_bt_latency *lat, uint64_t now)
{
	const unsigned int n = SPA_N_ELEMENTS(lat->impl.send);

	if (lat->disabled)
		return;

	lat->impl.send[lat->impl.pos++] = now;
	if (lat->impl.pos >= n)
		lat->impl.pos = 0;
}

static inline int spa_bt_latency_recv_errqueue(struct spa_bt_latency *lat, int fd, struct spa_log *log)
{
	struct {
		struct cmsghdr cm;
		char control[512];
	} control;

	if (lat->disabled)
		return -EOPNOTSUPP;

	do {
		struct iovec data = {
			.iov_base = NULL,
			.iov_len = 0
		};
		struct msghdr msg = {
			.msg_iov = &data,
			.msg_iovlen = 1,
			.msg_control = &control,
			.msg_controllen = sizeof(control),
		};
		struct cmsghdr *cmsg;
		struct scm_timestamping *tss = NULL;
		struct sock_extended_err *serr = NULL;
		int res;

		res = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING)
				tss = (void *)CMSG_DATA(cmsg);
			else if (cmsg->cmsg_level == SOL_BLUETOOTH && cmsg->cmsg_type == BT_SCM_ERROR)
				serr = (void *)CMSG_DATA(cmsg);
			else
				continue;
		}

		if (!tss || !serr || serr->ee_errno != ENOMSG || serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
			return -EINVAL;
		if (serr->ee_info != SCM_TSTAMP_SND)
			continue;

		struct timespec *ts = &tss->ts[0];
		int64_t tx_time = SPA_TIMESPEC_TO_NSEC(ts);
		uint32_t tx_pos = serr->ee_data % SPA_N_ELEMENTS(lat->impl.send);

		lat->value = tx_time - lat->impl.send[tx_pos];

		if (lat->impl.prev_tx && tx_time > lat->impl.prev_tx)
			spa_bt_ptp_update(&lat->ptp, lat->value, tx_time - lat->impl.prev_tx);

		lat->impl.prev_tx = tx_time;

		spa_log_trace(log, "fd:%d latency[%d] nsec:%"PRIu64" range:%d..%d ms",
				fd, tx_pos, lat->value,
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.min / SPA_NSEC_PER_MSEC : -1),
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.max / SPA_NSEC_PER_MSEC : -1));
	} while (true);

	lat->valid = spa_bt_ptp_valid(&lat->ptp);

	return 0;
}

static inline void spa_bt_latency_flush(struct spa_bt_latency *lat, int fd, struct spa_log *log)
{
	int so_timestamping = 0;

	/* Disable timestamping and flush errqueue */
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	spa_bt_latency_recv_errqueue(lat, fd, log);

	lat->disabled = true;
}

#endif
