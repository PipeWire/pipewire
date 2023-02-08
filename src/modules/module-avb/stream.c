/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include <spa/debug/mem.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>

#include "iec61883.h"
#include "stream.h"
#include "utils.h"
#include "aecp-aem-descriptors.h"

static void on_stream_destroy(void *d)
{
	struct stream *stream = d;
	spa_hook_remove(&stream->stream_listener);
	stream->stream = NULL;
}

static void on_source_stream_process(void *data)
{
	struct stream *stream = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t index, n_bytes;
	int32_t avail, wanted;

	if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	d = buf->buffer->datas;

	wanted = buf->requested ? buf->requested * stream->stride : d[0].maxsize;

	n_bytes = SPA_MIN(d[0].maxsize, (uint32_t)wanted);

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	if (avail < wanted) {
		pw_log_debug("capture underrun %d < %d", avail, wanted);
		memset(d[0].data, 0, n_bytes);
	} else {
		spa_ringbuffer_read_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				d[0].data, n_bytes);
		index += n_bytes;
		spa_ringbuffer_read_update(&stream->ring, index);
	}

	d[0].chunk->size = n_bytes;
	d[0].chunk->stride = stream->stride;
	d[0].chunk->offset = 0;
	buf->size = n_bytes / stream->stride;

	pw_stream_queue_buffer(stream->stream, buf);
}

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.process = on_source_stream_process
};

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static int flush_write(struct stream *stream, uint64_t current_time)
{
	int32_t avail;
	uint32_t index;
        uint64_t ptime, txtime;
	int pdu_count;
	ssize_t n;
	struct avb_frame_header *h = (void*)stream->pdu;
	struct avb_packet_iec61883 *p = SPA_PTROFF(h, sizeof(*h), void);
	uint8_t dbc;

	avail = spa_ringbuffer_get_read_index(&stream->ring, &index);

	pdu_count = (avail / stream->stride) / stream->frames_per_pdu;

	txtime = current_time + stream->t_uncertainty;
	ptime = txtime + stream->mtt;
	dbc = stream->dbc;

	while (pdu_count--) {
		*(uint64_t*)CMSG_DATA(stream->cmsg) = txtime;

		set_iovec(&stream->ring,
			stream->buffer_data,
			stream->buffer_size,
			index % stream->buffer_size,
			&stream->iov[1], stream->payload_size);

		p->seq_num = stream->pdu_seq++;
		p->tv = 1;
		p->timestamp = ptime;
		p->dbc = dbc;

		n = sendmsg(stream->source->fd, &stream->msg, MSG_NOSIGNAL);
		if (n < 0 || n != (ssize_t)stream->pdu_size) {
			pw_log_error("sendmsg() failed %zd != %zd: %m",
					n, stream->pdu_size);
		}
		txtime += stream->pdu_period;
		ptime += stream->pdu_period;
		index += stream->payload_size;
		dbc += stream->frames_per_pdu;
	}
	stream->dbc = dbc;
	spa_ringbuffer_read_update(&stream->ring, index);
	return 0;
}

static void on_sink_stream_process(void *data)
{
	struct stream *stream = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	int32_t filled;
	uint32_t index, offs, avail, size;
	struct timespec now;

	if ((buf = pw_stream_dequeue_buffer(stream->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	avail = size - offs;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);

	if (filled >= (int32_t)stream->buffer_size) {
		pw_log_warn("playback overrun %d >= %zd", filled, stream->buffer_size);
	} else {
		spa_ringbuffer_write_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				SPA_PTROFF(d[0].data, offs, void), avail);
		index += avail;
		spa_ringbuffer_write_update(&stream->ring, index);
	}
	pw_stream_queue_buffer(stream->stream, buf);

	clock_gettime(CLOCK_TAI, &now);
	flush_write(stream, SPA_TIMESPEC_TO_NSEC(&now));
}

static void setup_pdu(struct stream *stream)
{
	struct avb_frame_header *h;
	struct avb_packet_iec61883 *p;
	ssize_t payload_size, hdr_size, pdu_size;

	spa_memzero(stream->pdu, sizeof(stream->pdu));
	h = (struct avb_frame_header*)stream->pdu;
	p = SPA_PTROFF(h, sizeof(*h), void);

	hdr_size = sizeof(*h) + sizeof(*p);
	payload_size = stream->stride * stream->frames_per_pdu;
	pdu_size = hdr_size + payload_size;

	h->type = htons(0x8100);
	h->prio_cfi_id = htons((stream->prio << 13) | stream->vlan_id);
	h->etype = htons(0x22f0);

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		p->subtype = AVB_SUBTYPE_61883_IIDC;
		p->sv = 1;
		p->stream_id = htobe64(stream->id);
		p->data_len = htons(payload_size+8);
		p->tag = 0x1;
		p->channel = 0x1f;
		p->tcode = 0xa;
		p->sid = 0x3f;
		p->dbs = stream->info.info.raw.channels;
		p->qi2 = 0x2;
		p->format_id = 0x10;
		p->fdf = 0x2;
		p->syt = htons(0x0008);
	}
	stream->hdr_size = hdr_size;
	stream->payload_size = payload_size;
	stream->pdu_size = pdu_size;
}

static int setup_msg(struct stream *stream)
{
	stream->iov[0].iov_base = stream->pdu;
	stream->iov[0].iov_len = stream->hdr_size;
	stream->iov[1].iov_base = SPA_PTROFF(stream->pdu, stream->hdr_size, void);
	stream->iov[1].iov_len = stream->payload_size;
	stream->iov[2].iov_base = SPA_PTROFF(stream->pdu, stream->hdr_size, void);
	stream->iov[2].iov_len = 0;
	stream->msg.msg_name = &stream->sock_addr;
	stream->msg.msg_namelen = sizeof(stream->sock_addr);
	stream->msg.msg_iov = stream->iov;
	stream->msg.msg_iovlen = 3;
	stream->msg.msg_control = stream->control;
	stream->msg.msg_controllen = sizeof(stream->control);
	stream->cmsg = CMSG_FIRSTHDR(&stream->msg);
	stream->cmsg->cmsg_level = SOL_SOCKET;
	stream->cmsg->cmsg_type = SCM_TXTIME;
	stream->cmsg->cmsg_len = CMSG_LEN(sizeof(__u64));
	return 0;
}

static const struct pw_stream_events sink_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = on_stream_destroy,
	.process = on_sink_stream_process
};

struct stream *server_create_stream(struct server *server,
		enum spa_direction direction, uint16_t index)
{
	struct stream *stream;
	const struct descriptor *desc;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	int res;

	desc = server_find_descriptor(server,
			direction == SPA_DIRECTION_INPUT ?
			AVB_AEM_DESC_STREAM_INPUT :
			AVB_AEM_DESC_STREAM_OUTPUT, index);
	if (desc == NULL)
		return NULL;

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return NULL;

	stream->server = server;
	stream->direction = direction;
	stream->index = index;
	stream->desc = desc;
	spa_list_append(&server->streams, &stream->link);

	stream->prio = AVB_MSRP_PRIORITY_DEFAULT;
	stream->vlan_id = AVB_DEFAULT_VLAN;

	stream->id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)server->mac_addr[3] << 32 |
			(uint64_t)server->mac_addr[4] << 24 |
			(uint64_t)server->mac_addr[5] << 16 |
			htons(index);

	stream->vlan_attr = avb_mvrp_attribute_new(server->mvrp,
			AVB_MVRP_ATTRIBUTE_TYPE_VID);
	stream->vlan_attr->attr.vid.vlan = htons(stream->vlan_id);

	stream->buffer_data = calloc(1, BUFFER_SIZE);
	stream->buffer_size = BUFFER_SIZE;
	spa_ringbuffer_init(&stream->ring);

	if (direction == SPA_DIRECTION_INPUT) {
		stream->stream = pw_stream_new(server->impl->core, "source",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Source",
				PW_KEY_NODE_NAME, "avb.source",
				PW_KEY_NODE_DESCRIPTION, "AVB Source",
				PW_KEY_NODE_WANT_DRIVER, "true",
				NULL));
	} else {
		stream->stream = pw_stream_new(server->impl->core, "sink",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Audio/Sink",
				PW_KEY_NODE_NAME, "avb.sink",
				PW_KEY_NODE_DESCRIPTION, "AVB Sink",
				PW_KEY_NODE_WANT_DRIVER, "true",
				NULL));
	}
	if (stream->stream == NULL)
		goto error_free;

	pw_stream_add_listener(stream->stream,
			&stream->stream_listener,
			direction == SPA_DIRECTION_INPUT ?
				&source_stream_events :
				&sink_stream_events,
			stream);

	stream->info.info.raw.format = SPA_AUDIO_FORMAT_S24_32_BE;
	stream->info.info.raw.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
	stream->info.info.raw.rate = 48000;
	stream->info.info.raw.channels = 8;
	stream->stride = stream->info.info.raw.channels * 4;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &stream->info.info.raw);

	if ((res = pw_stream_connect(stream->stream,
			pw_direction_reverse(direction),
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_INACTIVE |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		goto error_free_stream;

	stream->frames_per_pdu = 6;
	stream->pdu_period = SPA_NSEC_PER_SEC * stream->frames_per_pdu /
                          stream->info.info.raw.rate;

	setup_pdu(stream);
	setup_msg(stream);

	stream->listener_attr = avb_msrp_attribute_new(server->msrp,
			AVB_MSRP_ATTRIBUTE_TYPE_LISTENER);
	stream->talker_attr = avb_msrp_attribute_new(server->msrp,
			AVB_MSRP_ATTRIBUTE_TYPE_TALKER_ADVERTISE);
	stream->talker_attr->attr.talker.vlan_id = htons(stream->vlan_id);
	stream->talker_attr->attr.talker.tspec_max_frame_size = htons(32 + stream->frames_per_pdu * stream->stride);
	stream->talker_attr->attr.talker.tspec_max_interval_frames =
		htons(AVB_MSRP_TSPEC_MAX_INTERVAL_FRAMES_DEFAULT);
	stream->talker_attr->attr.talker.priority = stream->prio;
	stream->talker_attr->attr.talker.rank = AVB_MSRP_RANK_DEFAULT;
	stream->talker_attr->attr.talker.accumulated_latency = htonl(95);

	return stream;

error_free_stream:
	pw_stream_destroy(stream->stream);
	errno = -res;
error_free:
	free(stream);
	return NULL;
}

void stream_destroy(struct stream *stream)
{
	avb_mrp_attribute_destroy(stream->listener_attr->mrp);
	spa_list_remove(&stream->link);
	free(stream);
}

static int setup_socket(struct stream *stream)
{
	struct server *server = stream->server;
	int fd, res;
	char buf[128];
	struct ifreq req;

	fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	res = ioctl(fd, SIOCGIFINDEX, &req);
	if (res < 0) {
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		res = -errno;
		goto error_close;
	}

	spa_zero(stream->sock_addr);
	stream->sock_addr.sll_family = AF_PACKET;
	stream->sock_addr.sll_protocol = htons(ETH_P_TSN);
	stream->sock_addr.sll_ifindex = req.ifr_ifindex;

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		struct sock_txtime txtime_cfg;

		res = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &stream->prio,
				sizeof(stream->prio));
		if (res < 0) {
			pw_log_error("setsockopt(SO_PRIORITY %d) failed: %m", stream->prio);
			res = -errno;
			goto error_close;
		}

		txtime_cfg.clockid = CLOCK_TAI;
		txtime_cfg.flags = 0;
		res = setsockopt(fd, SOL_SOCKET, SO_TXTIME, &txtime_cfg,
				sizeof(txtime_cfg));
		if (res < 0) {
			pw_log_error("setsockopt(SO_TXTIME) failed: %m");
			res = -errno;
			goto error_close;
		}
	} else {
		struct packet_mreq mreq;

		res = bind(fd, (struct sockaddr *) &stream->sock_addr, sizeof(stream->sock_addr));
		if (res < 0) {
			pw_log_error("bind() failed: %m");
			res = -errno;
			goto error_close;
		}

		spa_zero(mreq);
		mreq.mr_ifindex = req.ifr_ifindex;
		mreq.mr_type = PACKET_MR_MULTICAST;
		mreq.mr_alen = ETH_ALEN;
		memcpy(&mreq.mr_address, stream->addr, ETH_ALEN);
		res = setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
				&mreq, sizeof(struct packet_mreq));

		pw_log_info("join %s", avb_utils_format_addr(buf, 128, stream->addr));

		if (res < 0) {
			pw_log_error("setsockopt(ADD_MEMBERSHIP) failed: %m");
			res = -errno;
			goto error_close;
		}
	}
	return fd;

error_close:
	close(fd);
	return res;
}

static void handle_iec61883_packet(struct stream *stream,
		struct avb_packet_iec61883 *p, int len)
{
	uint32_t index, n_bytes;
	int32_t filled;

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	n_bytes = ntohs(p->data_len) - 8;

	if (filled + n_bytes > stream->buffer_size) {
		pw_log_debug("capture overrun");
	} else {
		spa_ringbuffer_write_data(&stream->ring,
				stream->buffer_data,
				stream->buffer_size,
				index % stream->buffer_size,
				p->payload, n_bytes);
		index += n_bytes;
		spa_ringbuffer_write_update(&stream->ring, index);
	}
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct stream *stream = data;

	if (mask & SPA_IO_IN) {
		int len;
		uint8_t buffer[2048];

		len = recv(fd, buffer, sizeof(buffer), 0);

		if (len < 0) {
			pw_log_warn("got recv error: %m");
		}
		else if (len < (int)sizeof(struct avb_packet_header)) {
			pw_log_warn("short packet received (%d < %d)", len,
					(int)sizeof(struct avb_packet_header));
		} else {
			struct avb_frame_header *h = (void*)buffer;
			struct avb_packet_iec61883 *p = SPA_PTROFF(h, sizeof(*h), void);

			if (memcmp(h->dest, stream->addr, 6) != 0 ||
			    p->subtype != AVB_SUBTYPE_61883_IIDC)
				return;

			handle_iec61883_packet(stream, p, len - sizeof(*h));
		}
	}
}

int stream_activate(struct stream *stream, uint64_t now)
{
	struct server *server = stream->server;
	struct avb_frame_header *h = (void*)stream->pdu;
	int fd, res;

	if (stream->source == NULL) {
		if ((fd = setup_socket(stream)) < 0)
			return fd;

		stream->source = pw_loop_add_io(server->impl->loop, fd,
				SPA_IO_IN, true, on_socket_data, stream);
		if (stream->source == NULL) {
			res = -errno;
			pw_log_error("stream %p: can't create source: %m", stream);
			close(fd);
			return res;
		}
	}

	avb_mrp_attribute_begin(stream->vlan_attr->mrp, now);
	avb_mrp_attribute_join(stream->vlan_attr->mrp, now, true);

	if (stream->direction == SPA_DIRECTION_INPUT) {
		stream->listener_attr->attr.listener.stream_id = htobe64(stream->peer_id);
		stream->listener_attr->param = AVB_MSRP_LISTENER_PARAM_READY;
		avb_mrp_attribute_begin(stream->listener_attr->mrp, now);
		avb_mrp_attribute_join(stream->listener_attr->mrp, now, true);

		stream->talker_attr->attr.talker.stream_id = htobe64(stream->peer_id);
		avb_mrp_attribute_begin(stream->talker_attr->mrp, now);
	} else {
		if ((res = avb_maap_get_address(server->maap, stream->addr, stream->index)) < 0)
			return res;

		stream->listener_attr->attr.listener.stream_id = htobe64(stream->id);
		stream->listener_attr->param = AVB_MSRP_LISTENER_PARAM_IGNORE;
		avb_mrp_attribute_begin(stream->listener_attr->mrp, now);

		stream->talker_attr->attr.talker.stream_id = htobe64(stream->id);
		memcpy(stream->talker_attr->attr.talker.dest_addr, stream->addr, 6);

		stream->sock_addr.sll_halen = ETH_ALEN;
		memcpy(&stream->sock_addr.sll_addr, stream->addr, ETH_ALEN);
		memcpy(h->dest, stream->addr, 6);
		memcpy(h->src, server->mac_addr, 6);
		avb_mrp_attribute_begin(stream->talker_attr->mrp, now);
		avb_mrp_attribute_join(stream->talker_attr->mrp, now, true);
	}
	pw_stream_set_active(stream->stream, true);
	return 0;
}

int stream_deactivate(struct stream *stream, uint64_t now)
{
	pw_stream_set_active(stream->stream, false);

	if (stream->source != NULL) {
		pw_loop_destroy_source(stream->server->impl->loop, stream->source);
		stream->source = NULL;
	}

	avb_mrp_attribute_leave(stream->vlan_attr->mrp, now);

	if (stream->direction == SPA_DIRECTION_INPUT) {
		avb_mrp_attribute_leave(stream->listener_attr->mrp, now);
	} else {
		avb_mrp_attribute_leave(stream->talker_attr->mrp, now);
	}
	return 0;
}
