/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

static void vban_midi_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t timestamp, duration, maxsize, read;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	struct spa_pod *pod;
	struct spa_pod_control *c;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	maxsize = d[0].maxsize;

	/* we always use the graph position to select events */
	if (impl->io_position) {
		duration = impl->io_position->clock.duration;
		timestamp = impl->io_position->clock.position;
	} else {
		duration = 8192;
		timestamp = 0;
	}

	/* we copy events into the buffer as they are available. */
	spa_pod_builder_init(&b, d[0].data, maxsize);
	spa_pod_builder_push_sequence(&b, &f[0], 0);

	while (true) {
		int32_t avail = spa_ringbuffer_get_read_index(&impl->ring, &read);
		if (avail <= 0)
			break;

		ptr = SPA_PTROFF(impl->buffer, read & BUFFER_MASK2, void);

		if ((pod = spa_pod_from_data(ptr, avail, 0, avail)) == NULL)
			goto done;
		if (!spa_pod_is_sequence(pod))
			goto done;

		/* the ringbuffer contains series of sequences, one for each
		 * received packet */
		SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
#if 0
			/* try to render with given delay */
			uint32_t target = c->offset + impl->target_buffer;
			target = (uint64_t)target * rate / impl->rate;
#else
			uint32_t target = timestamp;
#endif
			if (timestamp != 0) {
				/* skip old packets */
				if (target < timestamp)
					continue;
				/* event for next cycle */
				if (target >= timestamp + duration)
					goto complete;
			} else {
				timestamp = target;
			}
			spa_pod_builder_control(&b, target - timestamp, SPA_CONTROL_Midi);
			spa_pod_builder_bytes(&b,
					SPA_POD_BODY(&c->value),
					SPA_POD_BODY_SIZE(&c->value));
		}
		/* we completed a sequence (one RTP packet), advance ringbuffer
		 * and go to the next packet */
		read += SPA_PTRDIFF(c, ptr);
		spa_ringbuffer_read_update(&impl->ring, read);
	}
complete:
	spa_pod_builder_pop(&b, &f[0]);

	if (b.state.offset > maxsize) {
		pw_log_warn("overflow buffer %u %u", b.state.offset, maxsize);
		b.state.offset = 0;
	}
	d[0].chunk->size = b.state.offset;
	d[0].chunk->stride = 1;
	d[0].chunk->offset = 0;
done:
	pw_stream_queue_buffer(impl->stream, buf);
}

static int parse_varlen(uint8_t *p, uint32_t avail, uint32_t *result)
{
	uint32_t value = 0, offs = 0;
	while (offs < avail) {
		uint8_t b = p[offs++];
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0)
			break;
	}
	*result = value;
	return offs;
}

static int get_midi_size(uint8_t *p, uint32_t avail)
{
	int size;
	uint32_t offs = 0, value;

	switch (p[offs++]) {
	case 0xc0 ... 0xdf:
		size = 2;
		break;
	case 0x80 ... 0xbf:
	case 0xe0 ... 0xef:
		size = 3;
		break;
	case 0xff:
	case 0xf0:
	case 0xf7:
		size = parse_varlen(&p[offs], avail - offs, &value);
		size += value + 1;
		break;
	default:
		return -EINVAL;
	}
	return size;
}

static int vban_midi_receive_midi(struct impl *impl, uint8_t *packet,
		uint32_t payload_offset, uint32_t plen)
{
	uint32_t write;
	int32_t filled;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	uint32_t offs = payload_offset;
	uint32_t timestamp = 0;

	/* no sync, resync */
	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u", timestamp);
		impl->have_sync = true;
		impl->ring.readindex = impl->ring.writeindex;
	}

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write);
	if (filled > (int32_t)BUFFER_SIZE2) {
		pw_log_warn("overflow");
		return -ENOSPC;
	}

	ptr = SPA_PTROFF(impl->buffer, write & BUFFER_MASK2, void);

	/* each packet is written as a sequence of events. The offset is
	 * the receive timestamp */
	spa_pod_builder_init(&b, ptr, BUFFER_SIZE2 - filled);
	spa_pod_builder_push_sequence(&b, &f[0], 0);

	while (offs < plen) {
		int size;

		spa_pod_builder_control(&b, timestamp, SPA_CONTROL_Midi);

		size = get_midi_size(&packet[offs], plen - offs);

		if (size <= 0 || offs + size > plen) {
			pw_log_warn("invalid size (%08x) %d (%u %u)",
					packet[offs], size, offs, plen);
			break;
		}

		spa_pod_builder_bytes(&b, &packet[offs], size);

		offs += size;
	}
	spa_pod_builder_pop(&b, &f[0]);

	write += b.state.offset;
	spa_ringbuffer_write_update(&impl->ring, write);

	return 0;
}

static int vban_midi_receive(struct impl *impl, uint8_t *buffer, ssize_t len)
{
	struct vban_header *hdr;
	ssize_t hlen;
	uint32_t n_frames;

	if (len < VBAN_HEADER_SIZE)
		goto short_packet;

	hdr = (struct vban_header*)buffer;
	if (strncmp(hdr->vban, "VBAN", 3))
		goto invalid_version;

	hlen = VBAN_HEADER_SIZE;

	n_frames = hdr->n_frames;
	if (impl->have_sync && impl->n_frames != n_frames) {
		pw_log_info("unexpected frame (%d != %d)",
				n_frames, impl->n_frames);
		impl->have_sync = false;
	}
	impl->n_frames = n_frames + 1;

	impl->receiving = true;

	return vban_midi_receive_midi(impl, buffer, hlen, len);

short_packet:
	pw_log_warn("short packet received");
	return -EINVAL;
invalid_version:
	pw_log_warn("invalid RTP version");
	spa_debug_mem(0, buffer, len);
	return -EPROTO;
}

static void vban_midi_flush_packets(struct impl *impl,
		struct spa_pod_sequence *sequence, uint32_t timestamp, uint32_t rate)
{
	struct spa_pod_control *c;
	struct vban_header header;
	struct iovec iov[2];
	uint32_t len;

	header = impl->header;

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	iov[1].iov_base = impl->buffer;
	iov[1].iov_len = 0;

	len = 0;

	SPA_POD_SEQUENCE_FOREACH(sequence, c) {
		void *ev;
		uint32_t size;

		if (c->type != SPA_CONTROL_Midi)
			continue;

		ev = SPA_POD_BODY(&c->value),
                size = SPA_POD_BODY_SIZE(&c->value);
		if (len == 0) {
			/* start new packet */
			header.n_frames++;
		} else if (len + size > impl->mtu) {
			/* flush packet when we have one and when it's too large */
			iov[1].iov_len = len;

			pw_log_debug("sending %d", len);
			vban_stream_emit_send_packet(impl, iov, 2);
			len = 0;
		}
		memcpy(&impl->buffer[len], ev, size);
		len += size;
	}
	if (len > 0) {
		/* flush last packet */
		iov[1].iov_len = len;

		pw_log_debug("sending %d", len);
		vban_stream_emit_send_packet(impl, iov, 2);
	}
	impl->header.n_frames = header.n_frames;
}

static void vban_midi_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, timestamp, rate;
	struct spa_pod *pod;
	void *ptr;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);

	if (SPA_LIKELY(impl->io_position)) {
		rate = impl->io_position->clock.rate.denom;
		timestamp = impl->io_position->clock.position * impl->rate / rate;
	} else {
		rate = 10000;
		timestamp = 0;
	}

	ptr = SPA_PTROFF(d[0].data, offs, void);

	if ((pod = spa_pod_from_data(ptr, size, 0, size)) == NULL)
		goto done;
	if (!spa_pod_is_sequence(pod))
		goto done;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u n_frames:%u",
				timestamp, impl->n_frames);
		impl->have_sync = true;
	}

	vban_midi_flush_packets(impl, (struct spa_pod_sequence*)pod, timestamp, rate);

done:
	pw_stream_queue_buffer(impl->stream, buf);
}

static int vban_midi_init(struct impl *impl, enum spa_direction direction)
{
	if (direction == SPA_DIRECTION_INPUT)
		impl->stream_events.process = vban_midi_process_capture;
	else
		impl->stream_events.process = vban_midi_process_playback;
	impl->receive_vban = vban_midi_receive;
	return 0;
}
