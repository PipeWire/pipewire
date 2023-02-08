/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_STREAM_H
#define AVB_STREAM_H

#include <sys/socket.h>
#include <sys/types.h>
#include <linux/if_packet.h>
#include <net/if.h>

#include <spa/utils/ringbuffer.h>
#include <spa/param/audio/format.h>

#include <pipewire/pipewire.h>

#define BUFFER_SIZE	(1u<<16)
#define BUFFER_MASK	(BUFFER_SIZE-1)

struct stream {
	struct spa_list link;

	struct server *server;

	uint16_t direction;
	uint16_t index;
	const struct descriptor *desc;
	uint64_t id;
	uint64_t peer_id;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	uint8_t addr[6];
	struct spa_source *source;
	int prio;
	int vlan_id;
	int mtt;
	int t_uncertainty;
	uint32_t frames_per_pdu;
	int ptime_tolerance;

	uint8_t pdu[2048];
	size_t hdr_size;
	size_t payload_size;
	size_t pdu_size;
	int64_t pdu_period;
	uint8_t pdu_seq;
	uint8_t prev_seq;
	uint8_t dbc;

	struct iovec iov[3];
	struct sockaddr_ll sock_addr;
	struct msghdr msg;
	char control[CMSG_SPACE(sizeof(uint64_t))];
	struct cmsghdr *cmsg;

	struct spa_ringbuffer ring;
	void *buffer_data;
	size_t buffer_size;

	uint64_t format;
	uint32_t stride;
	struct spa_audio_info info;

	struct avb_msrp_attribute *talker_attr;
	struct avb_msrp_attribute *listener_attr;
	struct avb_mvrp_attribute *vlan_attr;
};

#include "msrp.h"
#include "mvrp.h"
#include "maap.h"

struct stream *server_create_stream(struct server *server,
		enum spa_direction direction, uint16_t index);

void stream_destroy(struct stream *stream);

int stream_activate(struct stream *stream, uint64_t now);
int stream_deactivate(struct stream *stream, uint64_t now);

#endif /* AVB_STREAM_H */
