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
#include <spa/utils/ratelimit.h>
#include <spa/support/log.h>

#include "defs.h"
#include "rate-control.h"

/* New kernel API */
#ifndef BT_SCM_ERROR
#define BT_SCM_ERROR 0x04
#endif

#define NEW_SOF_TIMESTAMPING_TX_COMPLETION	(1 << 18)
#define NEW_SCM_TSTAMP_COMPLETION		(SCM_TSTAMP_ACK + 1)

/**
 * Bluetooth latency tracking.
 */
struct spa_bt_latency
{
	uint64_t value;
	struct spa_bt_ptp ptp;
	bool valid;
	bool enabled;
	uint32_t queue;
	uint32_t kernel_queue;
	size_t unsent;

	struct {
		struct {
			int64_t send;
			uint32_t pos;
			size_t size;
			bool snd;
			bool completion;
		} pending[64];
		uint32_t pos;
		int64_t prev_tx;
		struct spa_ratelimit rate_limit;
	} impl;
};

static inline void spa_bt_latency_init(struct spa_bt_latency *lat, struct spa_bt_transport *transport,
		uint32_t period, struct spa_log *log)
{
	int so_timestamping = (NEW_SOF_TIMESTAMPING_TX_COMPLETION | SOF_TIMESTAMPING_TX_SOFTWARE |
			SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY);
	int res;

	spa_zero(*lat);

	lat->impl.rate_limit.interval = 5 * 60 * SPA_NSEC_PER_SEC;
	lat->impl.rate_limit.burst = 8;

	if (!transport->device->adapter->tx_timestamping_supported)
		return;

	res = setsockopt(transport->fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	if (res < 0) {
		spa_log_info(log, "setsockopt(SO_TIMESTAMPING) failed (kernel feature not enabled?): %d (%m)", errno);
		return;
	}

	/* Flush errqueue on start */
	do {
		res = recv(transport->fd, NULL, 0, MSG_ERRQUEUE | MSG_DONTWAIT | MSG_TRUNC);
	} while (res == 0);

	spa_bt_ptp_init(&lat->ptp, period, period / 2);

	lat->enabled = true;
}

static inline void spa_bt_latency_reset(struct spa_bt_latency *lat)
{
	lat->value = 0;
	lat->valid = false;
	spa_bt_ptp_init(&lat->ptp, lat->ptp.period, lat->ptp.period / 2);
}

static inline void spa_bt_latency_clear_pending(struct spa_bt_latency *lat, unsigned int i,
		bool snd, bool completion)
{
	i = i % SPA_N_ELEMENTS(lat->impl.pending);

	if (snd && lat->impl.pending[i].snd) {
		if (lat->kernel_queue)
			lat->kernel_queue--;

		lat->impl.pending[i].snd = false;
	}

	if (completion && lat->impl.pending[i].completion) {
		if (lat->unsent > lat->impl.pending[i].size)
			lat->unsent -= lat->impl.pending[i].size;
		else
			lat->unsent = 0;

		if (lat->queue > 0)
			lat->queue--;
		if (!lat->queue)
			lat->unsent = 0;

		lat->impl.pending[i].completion = false;
	}
}

static inline ssize_t spa_bt_send(int fd, const void *buf, size_t size,
		struct spa_bt_latency *lat, uint64_t now)
{
	ssize_t res = send(fd, buf, size, MSG_DONTWAIT | MSG_NOSIGNAL);

	if (!lat || !lat->enabled)
		return res;

	if (res >= 0) {
		uint32_t i = lat->impl.pos % SPA_N_ELEMENTS(lat->impl.pending);

		spa_bt_latency_clear_pending(lat, i, true, true);
		lat->impl.pending[i].send = now;
		lat->impl.pending[i].size = size;
		lat->impl.pending[i].pos = lat->impl.pos % UINT16_MAX;
		lat->impl.pending[i].snd = true;
		lat->impl.pending[i].completion = true;

		lat->impl.pos++;
		lat->queue++;
		lat->kernel_queue++;
		lat->unsent += size;
	}

	return res;
}

static inline int spa_bt_latency_recv_errqueue(struct spa_bt_latency *lat, int fd, int64_t now,
		struct spa_log *log)
{
	union {
		char buf[CMSG_SPACE(32 * sizeof(struct scm_timestamping))];
		struct cmsghdr align;
	} control;
	unsigned int i;

	if (!lat->enabled)
		return -EOPNOTSUPP;

	do {
		struct iovec data = {
			.iov_base = NULL,
			.iov_len = 0
		};
		struct msghdr msg = {
			.msg_iov = &data,
			.msg_iovlen = 1,
			.msg_control = control.buf,
			.msg_controllen = sizeof(control.buf),
		};
		struct cmsghdr *cmsg;
		bool have_tss = false, have_serr = false;
		struct scm_timestamping tss;
		struct sock_extended_err serr;
		int res;

		res = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
		if (res < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPING) {
				memcpy(&tss, CMSG_DATA(cmsg), sizeof(tss));
				have_tss = true;
			} else if (cmsg->cmsg_level == SOL_BLUETOOTH && cmsg->cmsg_type == BT_SCM_ERROR) {
				memcpy(&serr, CMSG_DATA(cmsg), sizeof(serr));
				have_serr = true;
			} else {
				continue;
			}
		}

		if (!have_tss || !have_serr || serr.ee_errno != ENOMSG || serr.ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
			continue;

		uint32_t tx_pos = serr.ee_data % SPA_N_ELEMENTS(lat->impl.pending);

		if (serr.ee_data % UINT16_MAX != lat->impl.pending[tx_pos].pos) {
			spa_log_debug(log, "fd:%d latency[%u] bad value %u", fd, tx_pos, serr.ee_data);
			continue;
		}

		switch (serr.ee_info) {
		case SCM_TSTAMP_SND:
			spa_bt_latency_clear_pending(lat, tx_pos, true, false);
			continue;
		case NEW_SCM_TSTAMP_COMPLETION:
			if (!lat->impl.pending[tx_pos].completion)
				continue;
			break;
		default:
			continue;
		}

		struct timespec *ts = &tss.ts[0];
		int64_t tx_time = SPA_TIMESPEC_TO_NSEC(ts);

		lat->value = tx_time - lat->impl.pending[tx_pos].send;
		if (lat->impl.prev_tx && tx_time > lat->impl.prev_tx)
			spa_bt_ptp_update(&lat->ptp, lat->value, tx_time - lat->impl.prev_tx);

		lat->impl.prev_tx = tx_time;

		spa_bt_latency_clear_pending(lat, tx_pos, false, true);

		spa_log_trace(log, "fd:%d latency[%d] nsec:%"PRIu64" range:%d..%d ms",
				fd, tx_pos, lat->value,
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.min / SPA_NSEC_PER_MSEC : -1),
				(int)(spa_bt_ptp_valid(&lat->ptp) ? lat->ptp.max / SPA_NSEC_PER_MSEC : -1));
	} while (true);

	/* Clear too old pending latencies. Controllers (eg. Intel AX210, Realtek 8761CU)
	 * have known firmware bugs where they fail to report ISO packet completions. This
	 * will cause completion timestamps to be missing, so we should try to recover
	 * from this. (Kernel as of v6.18 will eventually stop sending though as it will
	 * think buffers are full.)
	 */
	for (i = 0; i < SPA_N_ELEMENTS(lat->impl.pending); ++i) {
		if (lat->impl.pending[i].snd || lat->impl.pending[i].completion) {
			if (lat->impl.pending[i].send + SPA_NSEC_PER_SEC < now) {
				int suppressed;

				if ((suppressed = spa_ratelimit_test(&lat->impl.rate_limit, now)) >= 0)
					spa_log_warn(log, "Missing completion reports for packet (%d suppressed): "
							"Bluetooth adapter firmware bug?", suppressed);

				spa_log_trace(log, "fd:%d latency[%u] too late", fd, i);
				spa_bt_latency_clear_pending(lat, i, true, true);
			}
		}
	}

	lat->valid = spa_bt_ptp_valid(&lat->ptp);

	return 0;
}

static inline void spa_bt_latency_flush(struct spa_bt_latency *lat, int fd, struct spa_log *log)
{
	int so_timestamping = 0;

	if (!lat->enabled)
		return;

	/* Disable timestamping and flush errqueue */
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping, sizeof(so_timestamping));
	spa_bt_latency_recv_errqueue(lat, fd, 0, log);

	lat->enabled = false;
}

#endif
