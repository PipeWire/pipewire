/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <limits.h>

/* TODO: Direct timestamp mode here may require a rework. See audio.c for a reference.
 * Also check out the usage of actual_max_buffer_size in audio.c. */

static void rtp_midi_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t timestamp, duration, maxsize, read, rate;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	struct spa_pod *pod;
	struct spa_pod_control *c;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	maxsize = d[0].maxsize;

	/* we always use the graph position to select events, the receiver side is
	 * responsible for smoothing out the RTP timestamps to graph time */
	if (impl->io_position) {
		duration = impl->io_position->clock.duration;
		timestamp = impl->io_position->clock.position;
		rate = impl->io_position->clock.rate.denom;
	} else {
		duration = 8192;
		timestamp = 0;
		rate = impl->rate;
	}

	/* we copy events into the buffer based on the rtp timestamp + delay. */
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
		 * received packet. This is not in shared mem so we can safely use
		 * the iterators here. */
		SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence*)pod, c) {
			/* try to render with given delay */
			uint32_t target = c->offset + impl->target_buffer;
			target = (uint64_t)target * rate / impl->rate;
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
			spa_pod_builder_control(&b, target - timestamp, c->type);
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
	d[0].chunk->offset = 0;
	d[0].chunk->size = b.state.offset;
	d[0].chunk->stride = 1;
	d[0].chunk->flags = 0;
done:
	pw_stream_queue_buffer(impl->stream, buf);
}

static int parse_varlen(uint8_t *p, uint32_t avail, uint32_t *result)
{
	uint32_t value = 0, offs = 0;
	while (offs < avail) {
		uint8_t b = p[offs++];
		if (value > (UINT32_MAX >> 7))
			return -ERANGE;
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0) {
			*result = value;
			return offs;
		}
	}
	return -EINVAL;
}

static int get_midi_size(uint8_t *p, uint32_t avail)
{
	int size;
	uint32_t offs = 0, value;

	if (avail < 1)
		return -EINVAL;
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
		if ((size = parse_varlen(&p[offs], avail - offs, &value)) < 0)
			return size;
		if ((unsigned int)(INT_MAX - size - 1) > value)
			return -EINVAL;
		size += (int)value + 1;
		break;
	default:
		return -EINVAL;
	}
	return size;
}
static int parse_journal(struct impl *impl, uint8_t *packet, uint16_t seq, uint32_t len)
{
	struct rtp_midi_journal *j;

	if (len < sizeof(*j))
		return -EINVAL;
	j = (struct rtp_midi_journal*)packet;
	uint16_t seqnum = ntohs(j->checkpoint_seqnum);
	rtp_stream_emit_send_feedback(impl, seqnum);
	return 0;
}

static double get_time(struct impl *impl)
{
	uint64_t now;
	struct spa_io_position *pos;
	double t;

	now = pw_stream_get_nsec(impl->stream);
	if ((pos = impl->io_position) != NULL) {
		t = pos->clock.position / (double) pos->clock.rate.denom;
		t += (now - pos->clock.nsec) / (double)SPA_NSEC_PER_SEC;
	} else {
		t = now;
	}
	return t;
}

static int rtp_midi_receive_midi(struct impl *impl, uint8_t *packet, uint32_t timestamp,
		uint16_t seq, uint32_t payload_offset, uint32_t plen)
{
	uint32_t write;
	struct rtp_midi_header hdr;
	int32_t filled;
	struct spa_pod_builder b;
	struct spa_pod_frame f[1];
	void *ptr;
	uint32_t offs = payload_offset, len, end;
	bool first = true;

	if (plen <= payload_offset)
		return -EINVAL;
	if (impl->direct_timestamp) {
		/* in direct timestamp we attach the RTP timestamp directly on the
		 * midi events and render them in the corresponding cycle */
		if (!impl->have_sync) {
			pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u direct:%d",
				timestamp, seq, impl->ts_offset, impl->ssrc,
				impl->direct_timestamp);
			impl->have_sync = true;
		}
	} else {
		/* in non-direct timestamp mode, we relate the graph clock against
		 * the RTP timestamps */
		double ts = timestamp / (float) impl->rate;
		double t = get_time(impl);
		double elapsed, estimated, diff;

		/* the elapsed time between RTP timestamps */
		elapsed = ts - impl->last_timestamp;
		/* for that elapsed time, our clock should have advanced
		 * by this amount since the last estimation */
		estimated = impl->last_time + elapsed * impl->corr;
		/* calculate the diff between estimated and current clock time in
		 * samples */
		diff = (estimated - t) * impl->rate;

		/* no sync or we drifted too far, resync */
		if (!impl->have_sync || fabs(diff) > impl->target_buffer) {
			impl->corr = 1.0;
			spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 256, impl->rate);

			pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u direct:%d",
				timestamp, seq, impl->ts_offset, impl->ssrc,
				impl->direct_timestamp);
			impl->have_sync = true;
			impl->ring.readindex = impl->ring.writeindex;
		} else {
			/* update our new rate correction */
			impl->corr = spa_dll_update(&impl->dll, diff);
			/* our current time is now the estimated time */
			t = estimated;
		}
		pw_log_trace("%f %f %f %f", t, estimated, diff, impl->corr);

		timestamp = (uint32_t)(t * impl->rate);

		impl->last_timestamp = (float)ts;
		impl->last_time = (float)t;
	}

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write);
	if (filled > (int32_t)BUFFER_SIZE2) {
		pw_log_warn("overflow");
		return -ENOSPC;
	}

	SPA_STATIC_ASSERT(sizeof hdr == 2);
	memcpy(&hdr, &packet[offs++], 1);
	if (hdr.b) {
		if (offs >= plen) {
			pw_log_warn("invalid packet: no room for long length byte");
			return -EINVAL;
		}
		hdr.len_b = packet[offs++];
		len = (hdr.len << 8) | hdr.len_b;
	} else {
		hdr.len_b = 0;
		len = hdr.len;
	}
	if (plen - offs < len) {
		pw_log_warn("invalid packet %" PRIu64 " > %" PRIu32, (uint64_t)offs + len, plen);
		return -EINVAL;
	}
	end = len + offs;
	if (hdr.j)
		parse_journal(impl, &packet[end], seq, plen - end);

	ptr = SPA_PTROFF(impl->buffer, write & BUFFER_MASK2, void);

	/* each packet is written as a sequence of events. The offset is
	 * the RTP timestamp */
	spa_pod_builder_init(&b, ptr, BUFFER_SIZE2 - filled);
	spa_pod_builder_push_sequence(&b, &f[0], 0);

	while (offs < end) {
		uint32_t delta;
		int size;
		uint64_t state = 0;
		uint8_t *d;
		size_t s;

		if (first && !hdr.z)
			delta = 0;
		else {
			size = parse_varlen(&packet[offs], end - offs, &delta);
			if (size < 0) {
				pw_log_warn("invalid offset at offset %u", offs);
				return size;
			}
			offs += size;
		}
		timestamp += (uint32_t)(delta * impl->corr);

		size = get_midi_size(&packet[offs], end - offs);
		if (size <= 0 || (unsigned int)size > end - offs) {
			pw_log_warn("invalid size (%08x) %d (%u %u)",
					packet[offs], size, offs, end);
			return -EINVAL;
		}

		d = &packet[offs];
		s = size;
		while (s > 0) {
			uint32_t ump[4];
			int ump_size = spa_ump_from_midi(&d, &s, ump, sizeof(ump), 0, &state);
			if (ump_size <= 0)
				break;

			spa_pod_builder_control(&b, timestamp, SPA_CONTROL_UMP);
	                spa_pod_builder_bytes(&b, ump, ump_size);
		}
		offs += size;
		first = false;
	}
	if (spa_pod_builder_pop(&b, &f[0]) == NULL) {
		pw_log_warn("overflow");
		return -ENOSPC;
	}
	write += b.state.offset;
	spa_ringbuffer_write_update(&impl->ring, write);

	return 0;
}

static int rtp_midi_receive(struct impl *impl, uint8_t *buffer, ssize_t len,
			uint64_t current_time)
{
	struct rtp_header *hdr;
	ssize_t hlen;
	uint16_t seq;
	uint32_t timestamp;

	SPA_STATIC_ASSERT(sizeof(struct rtp_header) == 12);
	if (len < 12)
		goto short_packet;

	hdr = (struct rtp_header*)buffer;
	if (hdr->v != 2)
		goto invalid_version;

	hlen = 12 + hdr->cc * 4;
	if (hlen >= len)
		goto invalid_len;

	if (impl->have_ssrc && impl->ssrc != hdr->ssrc)
		goto unexpected_ssrc;
	impl->ssrc = hdr->ssrc;
	impl->have_ssrc = !impl->ignore_ssrc;

	seq = ntohs(hdr->sequence_number);
	if (impl->have_seq && impl->seq != seq) {
		pw_log_info("unexpected seq (%d != %d) SSRC:%u",
				seq, impl->seq, hdr->ssrc);
		impl->have_sync = false;
	}
	impl->seq = seq + 1;
	impl->have_seq = true;

	timestamp = ntohl(hdr->timestamp) - impl->ts_offset;

	impl->receiving = true;

	return rtp_midi_receive_midi(impl, buffer, timestamp, seq, hlen, len);

short_packet:
	pw_log_warn("short packet received");
	return -EINVAL;
invalid_version:
	pw_log_warn("invalid RTP version");
	spa_debug_log_mem(pw_log_get(), SPA_LOG_LEVEL_INFO, 0, buffer, len);
	return -EPROTO;
invalid_len:
	pw_log_warn("invalid RTP length");
	return -EINVAL;
unexpected_ssrc:
	if (!impl->fixed_ssrc) {
		/* We didn't have a configured SSRC, and there's more than one SSRC on
		* this address/port pair */
		pw_log_warn("unexpected SSRC (expected %u != %u)",
			impl->ssrc, hdr->ssrc);
	}
	return -EINVAL;
}

static int write_event(uint8_t *p, uint32_t buffer_size, uint32_t value, void *ev, uint32_t size)
{
        uint64_t buffer;
        uint8_t b;
	unsigned int count = 0;

	if (buffer_size <= size)
		return -ENOSPC;
        buffer = value & 0x7f;
        while ((value >>= 7)) {
		if (buffer > (UINT64_MAX >> 8))
			return -ERANGE;
                buffer <<= 8;
                buffer |= ((value & 0x7f) | 0x80);
        }
        do  {
                if (count >= buffer_size)
                        return -ENOSPC;
		b = buffer & 0xff;
                p[count++] = b;
                buffer >>= 8;
        } while (b & 0x80);

	if (buffer_size - size < count ||
	    count + size > (unsigned int)INT_MAX)
		return -ENOSPC;
	memcpy(&p[count], ev, size);
        return (int)(count + size);
}

static void rtp_midi_flush_packets(struct impl *impl,
		struct spa_pod_parser *parser, uint32_t timestamp, uint32_t rate)
{
	struct spa_pod_control c;
	const void *c_body;
	struct rtp_header header;
	struct rtp_midi_header midi_header;
	struct iovec iov[3];
	uint32_t len, prev_offset, base, max_size;

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	spa_zero(midi_header);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	iov[1].iov_base = &midi_header;
	iov[1].iov_len = sizeof(midi_header);
	iov[2].iov_base = impl->buffer;
	iov[2].iov_len = 0;

	prev_offset = len = base = 0;
	max_size = impl->payload_size - sizeof(midi_header);

	while (spa_pod_parser_get_control_body(parser, &c, &c_body) >= 0) {
		uint32_t delta, offset;
		uint8_t event[16];
		int size;
		size_t c_size = c.value.size;
		uint64_t state = 0;

		if (c.type != SPA_CONTROL_UMP)
			continue;

		while (c_size > 0) {
			size = spa_ump_to_midi((const uint32_t **)&c_body, &c_size, event, sizeof(event), &state);
			if (size <= 0)
				break;

			offset = c.offset * impl->rate / rate;

			if (len > 0 && (len + size > max_size ||
			    offset - base > impl->psamples)) {
				/* flush packet when we have one and when it's either
				 * too large or has too much data. */
				if (len < 16) {
					midi_header.b = 0;
					midi_header.len = len;
					iov[1].iov_len = sizeof(midi_header) - 1;
				} else {
					midi_header.b = 1;
					midi_header.len = (len >> 8) & 0xf;
					midi_header.len_b = len & 0xff;
					iov[1].iov_len = sizeof(midi_header);
				}
				iov[2].iov_len = len;

				pw_log_trace("sending %d timestamp:%d %u %u",
						len, timestamp + base,
						offset, impl->psamples);
				rtp_stream_emit_send_packet(impl, iov, 3);

				impl->seq++;
				len = 0;
			}
			if ((unsigned int)size > BUFFER_SIZE || len > BUFFER_SIZE - size) {
				pw_log_error("Buffer overflow prevented!");
				return; // FIXME: what to do instead?
			}
			if (len == 0) {
				/* start new packet */
				base = prev_offset = offset;
				header.sequence_number = htons(impl->seq);
				header.timestamp = htonl(impl->ts_offset + timestamp + base);

				memcpy(&impl->buffer[len], event, size);
				len += size;
			} else {
				delta = offset - prev_offset;
				prev_offset = offset;
				len += write_event(&impl->buffer[len], BUFFER_SIZE - len, delta, event, size);
			}
		}
	}
	if (len > 0) {
		/* flush last packet */
		if (len < 16) {
			midi_header.b = 0;
			midi_header.len = len;
			iov[1].iov_len = sizeof(midi_header) - 1;
		} else {
			midi_header.b = 1;
			midi_header.len = (len >> 8) & 0xf;
			midi_header.len_b = len & 0xff;
			iov[1].iov_len = sizeof(midi_header);
		}
		iov[2].iov_len = len;

		pw_log_trace("sending %d timestamp:%d", len, base);
		rtp_stream_emit_send_packet(impl, iov, 3);
		impl->seq++;
	}
}

static void rtp_midi_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t timestamp, rate;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct spa_pod_sequence seq;
	const void *seq_body;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	if (SPA_LIKELY(impl->io_position)) {
		rate = impl->io_position->clock.rate.denom;
		timestamp = impl->io_position->clock.position * impl->rate / rate;
	} else {
		rate = 10000;
		timestamp = 0;
	}


	spa_pod_parser_init_from_data(&parser, d[0].data, d[0].maxsize,
			d[0].chunk->offset, d[0].chunk->size);
	if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) < 0)
		goto done;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u",
				timestamp, impl->seq, impl->ts_offset, impl->ssrc);
		impl->have_sync = true;
	}

	rtp_midi_flush_packets(impl, &parser, timestamp, rate);

done:
	pw_stream_queue_buffer(impl->stream, buf);
}

static int rtp_midi_init(struct impl *impl, enum spa_direction direction)
{
	if (direction == SPA_DIRECTION_INPUT)
		impl->stream_events.process = rtp_midi_process_capture;
	else
		impl->stream_events.process = rtp_midi_process_playback;
	impl->receive_rtp = rtp_midi_receive;
	return 0;
}
