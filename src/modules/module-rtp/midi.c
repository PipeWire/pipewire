/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <limits.h>

static int parse_journal(struct impl *impl, uint8_t *packet, uint16_t seq, uint32_t len)
{
	struct rtp_midi_journal *j;

	if (len < sizeof(*j))
		return -EINVAL;
	j = (struct rtp_midi_journal*)packet;
	uint16_t seqnum = ntohs(j->checkpoint_seqnum);
	rtp_stream_call_send_feedback(impl, seqnum);
	return 0;
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
	case 0xf0:
	case 0xf7:
		while (++offs < avail) {
			if (p[offs] == 0xf0 || p[offs] == 0xf7)
				return offs+1;
		}
		return -EINVAL;
	case 0xff:
		if ((size = parse_varlen(&p[offs], avail - offs, &value)) < 0)
			return size;
		if (value > (unsigned int)(INT_MAX - size - 1))
			return -EINVAL;
		size += (int)value + 1;
		break;
	default:
		return -EINVAL;
	}
	return size;
}

/* read events beteen begin and end timestamp. */
static void midi_packet_buffer_read(struct impl *impl, uint32_t timestamp, uint32_t duration,
		uint32_t rate, struct spa_pod_builder *b)
{
	struct rtp_packet *p, *t;
	struct spa_pod_frame f[1];
	uint32_t ts_begin = SPA_SCALE32(timestamp, impl->rate, rate);
	uint32_t ts_end = SPA_SCALE32(timestamp + duration, impl->rate, rate);

	spa_pod_builder_push_sequence(b, &f[0], 0);

	spa_list_for_each_safe(p, t, &impl->queued, link) {
		uint32_t ts;
		uint32_t offs, plen, len, end;
		uint64_t base;
		uint8_t *packet;
		bool first = true;
		struct rtp_midi_header hdr;

		if (!spa_list_is_end(t, &impl->queued, link) &&
		    ((uint64_t)t->timestamp + impl->target_buffer) <= ts_begin)
			/* the next packet is too old, we can skip this one */
			continue;

		if (p->decoded == NULL) {
			offs = p->hlen;
			packet = p->data;
			plen = p->size;

			SPA_STATIC_ASSERT(sizeof hdr == 2);
			memcpy(&hdr, &packet[offs++], 1);
			if (hdr.b) {
				if (offs >= plen) {
					pw_log_warn("invalid packet: no room for long length byte");
					continue;
				}
				hdr.len_b = packet[offs++];
				len = (hdr.len << 8) | hdr.len_b;
			} else {
				hdr.len_b = 0;
				len = hdr.len;
			}
			if (plen - offs < len) {
				pw_log_warn("invalid packet %" PRIu64 " > %" PRIu32, (uint64_t)offs + len, plen);
				continue;
			}
			end = len + offs;
			if (hdr.j)
				parse_journal(impl, &packet[end], p->seq, plen - end);

			p->decoded = SPA_PTROFF(p->data, offs, void);
			p->decoded_len = len;
		}

		/* bring packet time to graph time */
		base = (uint64_t)p->timestamp + impl->target_buffer;
		if (base >= ts_end)
			break;

#if 0
		if (base + 8 * impl->target_buffer < ts_begin) {
			spa_list_remove(&p->link);
			spa_list_append(&impl->free, &p->link);
			continue;
		}
#endif


		memcpy(&hdr, p->data, 1);
		packet = p->decoded;
		end = p->decoded_len;
		offs = 0;

		while (offs < end) {
			uint32_t delta;
			int size, tail_trim = 0;

			if (first && !hdr.z)
				delta = 0;
			else {
				size = parse_varlen(&packet[offs], end - offs, &delta);
				if (size < 0) {
					pw_log_warn("invalid offset at offset %u/%u (%d): %s",
							offs, end, size, spa_strerror(size));
					spa_debug_mem(0, p->data, p->size);
					break;
				}
				offs += size;
			}
			//base += (uint32_t)(delta * impl->corr);
			base += delta;

			size = get_midi_size(&packet[offs], end - offs);
			if (size <= 0 || (unsigned int)size > end - offs) {
				pw_log_warn("invalid size (%08x) %d (%u %u)",
						packet[offs], size, offs, end);
				spa_debug_mem(0, p->data, p->size);
				break;
			}
			if (base >= ts_begin) {
				if (base >= ts_end)
					goto done;

				if ((packet[offs] == 0xf0 || packet[offs] == 0xf7) &&
				    packet[offs + size-1] == 0xf0)
					tail_trim++;

				ts = SPA_SCALE32(base - ts_begin, impl->rate, rate);
				ts = SPA_CLAMP(ts, 0u, duration);

				spa_pod_builder_control(b, ts, SPA_CONTROL_Midi);
				spa_pod_builder_bytes(b, &packet[offs], size - tail_trim);
			}
			offs += size;
			first = false;
		}
		if (offs >= end) {
			spa_list_remove(&p->link);
			spa_list_append(&impl->free, &p->link);
		}
	}
done:
	if (spa_pod_builder_pop(b, &f[0]) == NULL)
		pw_log_warn("overflow");
}


/* TODO: Direct timestamp mode here may require a rework. See audio.c for a reference.
 * Also check out the usage of actual_max_buffer_size in audio.c. */

static void rtp_midi_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t timestamp, duration, maxsize, rate;
	struct spa_pod_builder b;

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

	midi_packet_buffer_read(impl, timestamp, duration, rate, &b);

	if (b.state.offset > maxsize) {
		pw_log_warn("overflow buffer %u %u", b.state.offset, maxsize);
		b.state.offset = 0;
	}
	d[0].chunk->offset = 0;
	d[0].chunk->size = b.state.offset;
	d[0].chunk->stride = 1;
	d[0].chunk->flags = 0;

	pw_stream_queue_buffer(impl->stream, buf);
}

static double get_time(struct impl *impl, uint64_t current_time)
{
	struct spa_io_position *pos;
	double t;

	if ((pos = impl->io_position) != NULL) {
		t = pos->clock.position / (double) pos->clock.rate.denom;
		t += (current_time - pos->clock.nsec) / (double)SPA_NSEC_PER_SEC;
	} else {
		t = current_time;
	}
	return t;
}

static int rtp_midi_receive(struct impl *impl, struct rtp_packet *p,
		uint64_t current_time)
{
	if (impl->direct_timestamp) {
		/* in direct timestamp we attach the RTP timestamp directly on the
		 * midi events and render them in the corresponding cycle */
		if (!impl->have_sync) {
			pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u direct:%d",
				p->timestamp, p->seq, impl->ts_offset, impl->ssrc,
				impl->direct_timestamp);
			impl->have_sync = true;
		}
	} else {
		/* in non-direct timestamp mode, we relate the graph clock against
		 * the RTP timestamps */
		double ts = (double)p->timestamp / (double)impl->rate;
		double t = get_time(impl, current_time);
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
				p->timestamp, p->seq, impl->ts_offset, impl->ssrc,
				impl->direct_timestamp);
			impl->have_sync = true;
		} else {
			/* update our new rate correction */
			impl->corr = spa_dll_update(&impl->dll, diff);
			/* our current time is now the estimated time */
			t = estimated;
		}

		impl->last_timestamp = (double)ts;
		impl->last_time = (double)t;

		pw_log_trace_fp("%u %f %f %f %f %f %f %u", p->seq, t, ts, elapsed,
				estimated, diff, impl->corr, p->timestamp);

		p->timestamp = (uint32_t)(t * impl->rate);
	}
	return 0;
}

static int write_event(uint8_t *p, uint32_t buffer_size, uint32_t delta, const uint8_t *ev, uint32_t size)
{
	uint64_t buffer;
	uint8_t b;
	unsigned int count = 0;
	uint32_t total;

	total = size;
	if ((ev[0] == 0xf0 || ev[0] == 0xf7) && ev[size-1] != 0xf7)
		total++;

	if (buffer_size <= total)
		return -ENOSPC;
	buffer = delta & 0x7f;
	while ((delta >>= 7)) {
		if (buffer > (UINT64_MAX >> 8))
			return -ERANGE;
		buffer <<= 8;
		buffer |= ((delta & 0x7f) | 0x80);
	}
	do  {
		if (count >= buffer_size)
			return -ENOSPC;
		b = buffer & 0xff;
		p[count++] = b;
		buffer >>= 8;
	} while (b & 0x80);

	if (buffer_size - total < count ||
	    count + total > (unsigned int)INT_MAX)
		return -ENOSPC;
	memcpy(&p[count], ev, size);
	if (size < total)
		p[count+size] = 0xf0;
	return (int)(count + total);
}

static void queue_packet(struct impl *impl, struct iovec *iov, int n_iov)
{
	struct rtp_packet *p;
	int i;

	p = rtp_stream_get_free_packet((struct rtp_stream*)impl);
	for (i = 0; i < n_iov; i ++) {
		memcpy(SPA_PTROFF(p->data, p->size, void), iov[i].iov_base, iov[i].iov_len);
		p->size += iov[i].iov_len;
	}
	spa_list_remove(&p->link);
	spa_list_append(&impl->queued, &p->link);
	impl->num_queued++;
}

static void rtp_midi_queue_packets(struct impl *impl,
		struct spa_pod_parser *parser, uint32_t timestamp, uint32_t rate)
{
	struct spa_pod_control c;
	const void *c_body;
	struct rtp_header header;
	struct rtp_midi_header midi_header;
	struct iovec iov[3];
	uint32_t len, prev_offset, base, max_size;
	uint8_t buffer[impl->payload_size];

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	spa_zero(midi_header);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);
	iov[1].iov_base = &midi_header;
	iov[1].iov_len = sizeof(midi_header);
	iov[2].iov_base = buffer;
	iov[2].iov_len = 0;

	prev_offset = len = base = 0;
	max_size = impl->payload_size - sizeof(midi_header);

	while (spa_pod_parser_get_control_body(parser, &c, &c_body) >= 0) {
		uint32_t delta, offset;
		uint32_t size = c.value.size;
		const uint8_t *data = c_body;

		if (c.type != SPA_CONTROL_Midi)
			continue;

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

			pw_log_trace_fp("sending %d timestamp:%d %u %u",
					len, timestamp + base,
					offset, impl->psamples);

			queue_packet(impl, iov, 3);

			impl->seq++;
			len = 0;
		}
		if ((unsigned int)size > sizeof(buffer) || len > sizeof(buffer) - size) {
			pw_log_error("Buffer overflow prevented!");
			return; // FIXME: what to do instead?
		}
		if (len == 0) {
			/* start new packet */
			base = prev_offset = offset;
			header.sequence_number = htons(impl->seq);
			header.timestamp = htonl(impl->ts_offset + timestamp + base);

			memcpy(&buffer[len], data, size);
			len += size;
		} else {
			int res;
			delta = offset - prev_offset;
			prev_offset = offset;
			res = write_event(&buffer[len], sizeof(buffer) - len, delta, data, size);
			if (res < 0) {
				pw_log_warn("write_event error: %d", res);
				return;
			}
			len += res;
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

		pw_log_trace_fp("sending %d timestamp:%d", len, base);
		queue_packet(impl, iov, 3);
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
	struct rtp_packet *p, *t;

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

	rtp_midi_queue_packets(impl, &parser, timestamp, rate);

	spa_list_for_each_safe(p, t, &impl->queued, link) {
		struct iovec iov[1];
		iov[0].iov_base = p->data;
		iov[0].iov_len = p->size;

		rtp_stream_call_send_packet(impl, iov, 1);

		spa_list_remove(&p->link);
		spa_list_append(&impl->free, &p->link);
	}
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
