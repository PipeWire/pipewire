/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

static inline void
set_iovec(struct spa_ringbuffer *rbuf, void *buffer, uint32_t size,
		uint32_t offset, struct iovec *iov, uint32_t len)
{
	iov[0].iov_len = SPA_MIN(len, size - offset);
	iov[0].iov_base = SPA_PTROFF(buffer, offset, void);
	iov[1].iov_len = len - iov[0].iov_len;
	iov[1].iov_base = buffer;
}

static void ringbuffer_clear(struct spa_ringbuffer *rbuf SPA_UNUSED,
			 void *buffer, uint32_t size,
			 uint32_t offset, uint32_t len)
{
	struct iovec iov[2];
	set_iovec(rbuf, buffer, size, offset, iov, len);
	memset(iov[0].iov_base, 0, iov[0].iov_len);
	memset(iov[1].iov_base, 0, iov[1].iov_len);
}

static void rtp_audio_process_playback(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	struct pw_time pwt;
	uint32_t wanted, timestamp, target_buffer, stride, maxsize;
	uint32_t device_delay;
	int32_t avail, flags = 0;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	stride = impl->stride;

	maxsize = d[0].maxsize / stride;
	wanted = buf->requested ? SPA_MIN(buf->requested, maxsize) : maxsize;

	pw_stream_get_time_n(impl->stream, &pwt, sizeof(pwt));

	/* Negative delay is used rarely, mostly for the combine stream.
	 * There, the delay is used as an offset value between streams.
	 * Here, negative delay values make no sense. It is safe to clamp
	 * delay values to 0 (see docs), so do that here. */
	device_delay = SPA_MAX(pwt.delay, 0LL);

	/* IMPORTANT: In the explanations below, sometimes, "reading/writing from/to the
	 * ring buffer at a position X" is mentioned. To be exact, that buffer is actually
	 * impl->buffer. And since X can be a timestamp whose value is far higher than the
	 * buffer size (and the fact that impl->buffer is a _ring_ buffer), reads and writes
	 * actually first apply BUFFER_MASK to the position to implement a ring buffer
	 * index wrap-around. (Wrap-around when reading / writing the data bytes is
	 * handled by the spa_ringbuffer code; this is about the wrap around of the
	 * read or write index itself.) */

	if (impl->direct_timestamp) {
		/* In direct timestamp mode, the focus lies on synchronized playback, not
		 * on a constant latency. The ring buffer fill level is not of interest
		 * here. The code in rtp_audio_receive() writes to the ring buffer at
		 * position (RTP timestamp + target_buffer), just like in the constant
		 * latency mode. Crucially however, in direct timestamp mode, it is assumed
		 * that the RTP timestamps are based on the same synchronized clock that
		 * runs the graph driver here, so the clock position is using the same
		 * time base as these timestamps.
		 *
		 * If the transport delay from the sender to this receiver were zero, then
		 * the data with the given RTP timestamp could in theory be played right
		 * away, since that timestamp would equal the clock position (or, in other
		 * words, it would be the present time). Since the transport takes some
		 * time, writing the data at the position (RTP timestamp + target_buffer)
		 * shifts the timestamp into the future sufficiently enough that no data
		 * is lost. (target_buffer corresponds to the `sess.latency.msec` RTP
		 * source module option, and that option has to be chosen by the user
		 * to be of a sensible size - high enough to at least match the maximum
		 * transport delay, but not too high to not risk too much latency
		 * Also, `sess.latency.msec` must be the same value across all RTP
		 * source nodes that shall play in sync.)
		 *
		 * When the code here reads from the position defined by the current
		 * clock position, it is then guaranteed that the data is accessed in
		 * sync with other RTP source nodes which also run in the direct
		 * timestamp mode, since all of them shift the timestamp by the same
		 * `sess.latency.msec` into the future.
		 *
		 * "Fill level" makes no sense in this mode, since a constant latency
		 * is not important in this mode, so no DLL is needed. Also, matching
		 * the pace of the synchronized clock is done by having the graph
		 * driver be synchronized to that clock, which will in turn cause
		 * any output sinks to adjust their DLLs (or similar control loop
		 * mechanisms) to match the pace of their data consumption with the
		 * pace of the driver. */

		if (impl->io_position) {
			/* Shift clock position by stream delay to compensate
			 * for processing and output delay. */
			timestamp = impl->io_position->clock.position + device_delay;
			spa_ringbuffer_read_update(&impl->ring, timestamp);
		} else {
			/* In the unlikely case that no spa_io_position pointer
			 * was passed yet by PipeWire to this node, resort to a
			 * default behavior: just use the current read index.
			 * This most likely is not in sync with other nodes,
			 * but _something_ is needed as read index until the
			 * spa_io_position is available. */
			spa_ringbuffer_get_read_index(&impl->ring, &timestamp);
		}

		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(timestamp * stride) & BUFFER_MASK,
				d[0].data, wanted * stride);

		/* Clear the bytes that were just retrieved. Since the fill level
		 * is not tracked in this buffer mode, it is possible that as soon
		 * as actual playback ends, the RTP source node re-reads old data.
		 * Make sure it reads silence when no actual new data is present
		 * and the RTP source node still runs. Do this by filling the
		 * region of the retrieved data with null bytes. */
		ringbuffer_clear(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(timestamp * stride) & BUFFER_MASK,
				wanted * stride);

		if (!impl->io_position) {
			/* In the unlikely case that no spa_io_position pointer
			 * was passed yet by PipeWire to this node, monotonically
			 * increment the read index like this to not consume from
			 * the same position in the ring buffer over and over again. */
			timestamp += wanted;
			spa_ringbuffer_read_update(&impl->ring, timestamp);
		}
	} else {
		/* In the constant delay mode, it is assumed that the ring buffer fill
		 * level matches impl->target_buffer. If not, check for over- and
		 * underruns. Adjust the DLL as needed. If the over/underruns are too
		 * severe, resynchronize. */

		avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);

		/* Reduce target buffer by the delay amount to start playback sooner.
		 * This compensates for the delay to the device. */
		if (SPA_UNLIKELY(impl->target_buffer < device_delay)) {
			pw_log_error("Delay to device (%" PRIu32 ") is higher than "
				"the target buffer size (%" PRIu32 ")", device_delay,
				impl->target_buffer);
			target_buffer = 0;
		} else {
			target_buffer = impl->target_buffer - device_delay;
		}

		if (avail < (int32_t)wanted) {
			enum spa_log_level level;
			memset(d[0].data, 0, wanted * stride);
			flags |= SPA_CHUNK_FLAG_EMPTY;

			if (impl->have_sync) {
				impl->have_sync = false;
				level = SPA_LOG_LEVEL_INFO;
			} else {
				level = SPA_LOG_LEVEL_DEBUG;
			}
			pw_log(level, "receiver read underrun %d/%u < %u",
						avail, target_buffer, wanted);
		} else {
			double error, corr;
			if (impl->first) {
				if ((uint32_t)avail > target_buffer) {
					uint32_t skip = avail - target_buffer;
					pw_log_debug("first: avail:%d skip:%u target:%u",
								avail, skip, target_buffer);
					timestamp += skip;
					avail = target_buffer;
				}
				impl->first = false;
			} else if (avail > (int32_t)SPA_MIN(target_buffer * 8, BUFFER_SIZE / stride)) {
				pw_log_warn("receiver read overrun %u > %u", avail, target_buffer * 8);
				timestamp += avail - target_buffer;
				avail = target_buffer;
			}

			/* when the speed of the sender clock and our clock are
			 * not in sync, try to adjust our playback rate to keep
			 * the requested target_buffer bytes in the ringbuffer */
			double in_flight = 0;
			struct spa_io_position *pos = impl->io_position;

			if (SPA_LIKELY(pos && impl->last_recv_timestamp)) {
				/* Account for samples that might be in flight but not yet received, and possibly
				 * samples that were received _after_ the process() tick and therefore should not
				 * yet be accounted for */
				int64_t in_flight_ns = pos->clock.nsec - impl->last_recv_timestamp;
				/* Use the best relative rate we know */
				double relative_rate = impl->io_rate_match ? impl->io_rate_match->rate : pos->clock.rate_diff;
				in_flight = (double)(in_flight_ns * impl->rate) * relative_rate / SPA_NSEC_PER_SEC;
			}

			error = (double)target_buffer - (double)avail - in_flight;
			error = SPA_CLAMPD(error, -impl->max_error, impl->max_error);

			corr = spa_dll_update(&impl->dll, error);

			pw_log_trace("avail:%u target:%u error:%f corr:%f", avail,
					target_buffer, error, corr);

			pw_stream_set_rate(impl->stream, 1.0 / corr);

			spa_ringbuffer_read_data(&impl->ring,
					impl->buffer,
					BUFFER_SIZE,
					(timestamp * stride) & BUFFER_MASK,
					d[0].data, wanted * stride);

			timestamp += wanted;
			spa_ringbuffer_read_update(&impl->ring, timestamp);
		}
	}

	d[0].chunk->offset = 0;
	d[0].chunk->size = wanted * stride;
	d[0].chunk->stride = stride;
	d[0].chunk->flags = flags;
	buf->size = wanted;

	pw_stream_queue_buffer(impl->stream, buf);
}

static int rtp_audio_receive(struct impl *impl, uint8_t *buffer, ssize_t len)
{
	struct rtp_header *hdr;
	ssize_t hlen, plen;
	uint16_t seq;
	uint32_t timestamp, samples, write, expected_write;
	uint32_t stride = impl->stride;
	int32_t filled;

	if (len < 12)
		goto short_packet;

	hdr = (struct rtp_header*)buffer;
	if (hdr->v != 2)
		goto invalid_version;

	hlen = 12 + hdr->cc * 4;
	if (hlen > len)
		goto invalid_len;

	if (impl->have_ssrc && impl->ssrc != hdr->ssrc)
		goto unexpected_ssrc;
	impl->ssrc = hdr->ssrc;
	impl->have_ssrc = !impl->ignore_ssrc;

	seq = ntohs(hdr->sequence_number);
	if (impl->have_seq && impl->seq != seq) {
		pw_log_info("unexpected seq (%d != %d) SSRC:%u",
				seq, impl->seq, hdr->ssrc);
		/* No need to resynchronize here. If packets arrive out of
		 * order, then they are still written in order into the ring
		 * buffer, since they are written according to where the
		 * RTP timestamp points to. */
	}
	impl->seq = seq + 1;
	impl->have_seq = true;

	timestamp = ntohl(hdr->timestamp) - impl->ts_offset;

	impl->receiving = true;
	impl->last_recv_timestamp = pw_stream_get_nsec(impl->stream);

	plen = len - hlen;
	samples = plen / stride;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_write);

	/* we always write to timestamp + delay */
	write = timestamp + impl->target_buffer;

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u target:%u direct:%u",
				timestamp, seq, impl->ts_offset, impl->ssrc,
				impl->target_buffer, impl->direct_timestamp);

		/* we read from timestamp, keeping target_buffer of data
		 * in the ringbuffer. */
		impl->ring.readindex = timestamp;
		impl->ring.writeindex = write;
		filled = impl->target_buffer;

		spa_dll_init(&impl->dll);
		spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
	} else if (expected_write != write) {
		pw_log_debug("unexpected write (%u != %u)",
				write, expected_write);
	}

	/* Write overrun only makes sense in constant delay mode. See the
	 * RTP source module documentation and the rtp_audio_process_playback()
	 * code for an explanation why. */
	if (!impl->direct_timestamp && (filled + samples > BUFFER_SIZE / stride)) {
		pw_log_debug("receiver write overrun %u + %u > %u", filled, samples,
				BUFFER_SIZE / stride);
		impl->have_sync = false;
	} else {
		pw_log_trace("got samples:%u", samples);
		spa_ringbuffer_write_data(&impl->ring,
				impl->buffer,
				BUFFER_SIZE,
				(write * stride) & BUFFER_MASK,
				&buffer[hlen], (samples * stride));

		/* Only update the write index if data was actually _appended_.
		 * If packets arrived out of order, then it may be that parts
		 * of the ring buffer further ahead were written to first, and
		 * now, unwritten parts preceding those other parts were now
		 * written to. For example, if previously, 10 samples were
		 * written to index 100, even though 10 samples were expected
		 * to be written at index 90, then there is a "hole" at index
		 * 90. If now, the packet that contains data for index 90
		 * arrived, then this data will be _inserted_ at index 90,
		 * and not _appended_. In this example, `expected_write` would
		 * be 100 (since `expected_write` is the current write index),
		 * `write` would be 90, `samples` would be 10. In this case,
		 * the inequality below does not hold, so data is being
		 * _inserted_. By contrast, during normal operation, `write`
		 * and `expected_write` are equal, so the inequality below
		 * _does_ hold, meaning that data is being appended.
		 *
		 * (Note that this write index update is only important if
		 * the constant delay mode is active, or if no spa_io_position
		 * was provided yet. See the rtp_audio_process_playback()
		 * code for more about this.) */
		if (expected_write < (write + samples)) {
			write += samples;
			spa_ringbuffer_write_update(&impl->ring, write);
		}
	}

	return 0;

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
		pw_log_warn("unexpected SSRC (expected %u != %u)", impl->ssrc,
			hdr->ssrc);
	}
	return -EINVAL;
}

static void set_timer(struct impl *impl, uint64_t time, uint64_t itime)
{
	struct itimerspec ts;
	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = itime / SPA_NSEC_PER_SEC;
	ts.it_interval.tv_nsec = itime % SPA_NSEC_PER_SEC;
	spa_system_timerfd_settime(impl->data_loop->system,
			impl->timer->fd, SPA_FD_TIMER_ABSTIME, &ts, NULL);
	impl->timer_running = time != 0 && itime != 0;
}

static void rtp_audio_flush_packets(struct impl *impl, uint32_t num_packets, uint64_t set_timestamp)
{
	int32_t avail, tosend;
	uint32_t stride, timestamp;
	struct iovec iov[3];
	struct rtp_header header;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &timestamp);
	tosend = impl->psamples;
	if (avail < tosend)
		if (impl->started)
			goto done;
		else {
			/* send last packet before emitting state_changed */
			tosend = avail;
			num_packets = 1;
		}
	else
		num_packets = SPA_MIN(num_packets, (uint32_t)(avail / tosend));

	stride = impl->stride;

	spa_zero(header);
	header.v = 2;
	header.pt = impl->payload;
	header.ssrc = htonl(impl->ssrc);

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	while (num_packets > 0) {
		if (impl->marker_on_first && impl->first)
			header.m = 1;
		else
			header.m = 0;
		header.sequence_number = htons(impl->seq);
		header.timestamp = htonl(impl->ts_offset + (set_timestamp ? set_timestamp : timestamp));

		set_iovec(&impl->ring,
			impl->buffer, BUFFER_SIZE,
			(timestamp * stride) & BUFFER_MASK,
			&iov[1], tosend * stride);

		pw_log_trace("sending %d packet:%d ts_offset:%d timestamp:%d",
				tosend, num_packets, impl->ts_offset, timestamp);

		rtp_stream_emit_send_packet(impl, iov, 3);

		impl->seq++;
		impl->first = false;
		timestamp += tosend;
		avail -= tosend;
		num_packets--;
	}
	spa_ringbuffer_read_update(&impl->ring, timestamp);
done:
	if (impl->timer_running) {
		if (impl->started) {
			if (avail < tosend) {
				set_timer(impl, 0, 0);
			}
		} else if (avail <= 0) {
			bool started = false;

			/* the stream has been stopped and all packets have been sent */
			set_timer(impl, 0, 0);
			pw_loop_invoke(impl->main_loop, do_emit_state_changed, SPA_ID_INVALID, &started, sizeof started, false, impl);
		}
	}
}

static void rtp_audio_flush_timeout(struct impl *impl, uint64_t expirations)
{
	if (expirations > 1)
		pw_log_warn("missing timeout %"PRIu64, expirations);
	rtp_audio_flush_packets(impl, expirations, 0);
}

static void rtp_audio_process_capture(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t offs, size, actual_timestamp, expected_timestamp, stride;
	int32_t filled, wanted;
	uint32_t pending, num_queued;
	struct spa_io_position *pos;
	uint64_t next_nsec, quantum;

	if (impl->separate_sender) {
		/* apply the DLL rate */
		pw_stream_set_rate(impl->stream, impl->ptp_corr);
	}

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_info("Out of stream buffers: %m");
		return;
	}
	d = buf->buffer->datas;

	offs = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
	size = SPA_MIN(d[0].chunk->size, d[0].maxsize - offs);
	stride = impl->stride;
	wanted = size / stride;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &expected_timestamp);

	pos = impl->io_position;
	if (SPA_LIKELY(pos)) {
		uint32_t rate = pos->clock.rate.denom;
		actual_timestamp = pos->clock.position * impl->rate / rate;
		next_nsec = pos->clock.next_nsec;
		quantum = (uint64_t)(pos->clock.duration * SPA_NSEC_PER_SEC / (rate * pos->clock.rate_diff));

		if (impl->separate_sender) {
			/* the sender process() function uses this for managing the DLL */
			impl->sink_nsec = pos->clock.nsec;
			impl->sink_next_nsec = pos->clock.next_nsec;
			impl->sink_resamp_delay = impl->io_rate_match->delay;
			impl->sink_quantum = (uint64_t)(pos->clock.duration * SPA_NSEC_PER_SEC / rate);
		}
	} else {
		actual_timestamp = expected_timestamp;
		next_nsec = 0;
		quantum = 0;
	}

	if (!impl->have_sync) {
		pw_log_info("sync to timestamp:%u seq:%u ts_offset:%u SSRC:%u",
				actual_timestamp, impl->seq, impl->ts_offset, impl->ssrc);
		impl->ring.readindex = impl->ring.writeindex = actual_timestamp;
		memset(impl->buffer, 0, BUFFER_SIZE);
		impl->have_sync = true;
		expected_timestamp = actual_timestamp;
		filled = 0;

		if (impl->separate_sender) {
			/* the sender should know that the sync state has changed, and that it should
			 * refill the buffer */
			impl->refilling = true;
		}
	} else {
		if (SPA_ABS((int)expected_timestamp - (int)actual_timestamp) > (int)quantum) {
			pw_log_warn("timestamp: expected %u != actual %u", expected_timestamp, actual_timestamp);
			impl->have_sync = false;
		} else if (filled + wanted > (int32_t)SPA_MIN(impl->target_buffer * 8, BUFFER_SIZE / stride)) {
			pw_log_warn("sender write overrun %u + %u > %u/%u", filled, wanted,
					impl->target_buffer * 8, BUFFER_SIZE / stride);
			impl->have_sync = false;
			filled = 0;
		}
	}

	pw_log_trace("writing %u samples at %u", wanted, expected_timestamp);

	spa_ringbuffer_write_data(&impl->ring,
			impl->buffer,
			BUFFER_SIZE,
			(expected_timestamp * stride) & BUFFER_MASK,
			SPA_PTROFF(d[0].data, offs, void), wanted * stride);
	expected_timestamp += wanted;
	spa_ringbuffer_write_update(&impl->ring, expected_timestamp);

	pw_stream_queue_buffer(impl->stream, buf);

	if (impl->separate_sender) {
		/* sending will happen in a separate process() */
		return;
	}

	pending = filled / impl->psamples;
	num_queued = (filled + wanted) / impl->psamples;

	if (num_queued > 0) {
		/* flush all previous packets plus new one right away */
		rtp_audio_flush_packets(impl, pending + 1, 0);
		num_queued -= SPA_MIN(num_queued, pending + 1);

		if (num_queued > 0) {
			/* schedule timer for remaining */
			int64_t interval = quantum / (num_queued + 1);
			uint64_t time = next_nsec - num_queued * interval;
			pw_log_trace("%u %u %"PRIu64" %"PRIu64, pending, num_queued, time, interval);
			set_timer(impl, time, interval);
		}
	}
}

static void ptp_sender_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->ptp_sender_listener);
	impl->ptp_sender = NULL;
}

static void ptp_sender_process(void *d, struct spa_io_position *position)
{
	struct impl *impl = d;
	uint64_t nsec, next_nsec, quantum, quantum_nsec;
	uint32_t ptp_timestamp, rtp_timestamp, read_idx;
	uint32_t rate;
	uint32_t filled;
	double error, in_flight, delay;

	nsec = position->clock.nsec;
	next_nsec = position->clock.next_nsec;

	/* the ringbuffer indices are in sink timetamp domain */
	filled = spa_ringbuffer_get_read_index(&impl->ring, &read_idx);

	if (SPA_LIKELY(position)) {
		rate = position->clock.rate.denom;
		quantum = position->clock.duration;
		quantum_nsec = (uint64_t)(quantum * SPA_NSEC_PER_SEC / rate);
		/* PTP time tells us what time it is */
		ptp_timestamp = position->clock.position * impl->rate / rate;
		/* RTP time is based on when we sent the first packet after the last sync */
		rtp_timestamp = impl->rtp_base_ts + read_idx;
	} else {
		pw_log_warn("No clock information, skipping");
		return;
	}

	pw_log_trace("sink nsec:%"PRIu64", sink next_nsec:%"PRIu64", ptp nsec:%"PRIu64", ptp next_sec:%"PRIu64,
			impl->sink_nsec, impl->sink_next_nsec, nsec, next_nsec);

	/* If send is lagging by more than 2 or more quanta, reset */
	if (!impl->refilling && impl->rtp_last_ts &&
			SPA_ABS((int32_t)ptp_timestamp - (int32_t)impl->rtp_last_ts) >= (int32_t)(2 * quantum)) {
		pw_log_warn("expected %u - timestamp %u = %d >= 2 * %"PRIu64" quantum", rtp_timestamp, impl->rtp_last_ts,
				(int)ptp_timestamp - (int)impl->rtp_last_ts, quantum);
		goto resync;
	}

	if (!impl->have_sync) {
		pw_log_trace("Waiting for sync");
		return;
	}

	in_flight = (double)impl->sink_quantum * impl->rate / SPA_NSEC_PER_SEC *
		(double)(nsec - impl->sink_nsec) / (impl->sink_next_nsec - impl->sink_nsec);
	delay = filled + in_flight + impl->sink_resamp_delay;

	/* Make sure the PTP node wake up times are within the bounds of sink
	 * node wake up times (with a little bit of tolerance). */
	if (SPA_LIKELY(nsec > impl->sink_nsec - quantum_nsec &&
				nsec < impl->sink_next_nsec + quantum_nsec)) {
		/* Start adjusting if we're at/past the target delay. We requested ~1/2 the buffer
		 * size as the sink latency, so doing so ensures that we have two sink quanta of
		 * data, making the chance of and underrun low even for small buffer values */
		if (impl->refilling && (double)impl->target_buffer - delay <= 0) {
			impl->refilling = false;
			/* Store the offset for the PTP time at which we start sending */
			impl->rtp_base_ts = ptp_timestamp - read_idx;
			rtp_timestamp = impl->rtp_base_ts + read_idx; /* = ptp_timestamp */
			pw_log_debug("start sending. sink quantum:%"PRIu64", ptp quantum:%"PRIu64"", impl->sink_quantum, quantum_nsec);
		}

		if (!impl->refilling) {
			/*
			 * As per Controlling Adaptive Resampling paper[1], maintain
			 * W(t) - R(t) - delta = 0. We keep delta as target_buffer.
			 *
			 * [1] http://kokkinizita.linuxaudio.org/papers/adapt-resamp.pdf
			 */
			error = delay - impl->target_buffer;
			error = SPA_CLAMPD(error, -impl->max_error, impl->max_error);
			impl->ptp_corr = spa_dll_update(&impl->ptp_dll, error);

			pw_log_debug("filled:%u in_flight:%g delay:%g target:%u error:%f corr:%f",
					filled, in_flight, delay, impl->target_buffer, error, impl->ptp_corr);

			if (filled >= impl->psamples) {
				rtp_audio_flush_packets(impl, 1, rtp_timestamp);
				impl->rtp_last_ts = rtp_timestamp;
			}
		}
	} else {
		pw_log_warn("PTP node wake up time out of bounds !(%"PRIu64" < %"PRIu64" < %"PRIu64")",
				impl->sink_nsec, nsec, impl->sink_next_nsec);
		goto resync;
	}

	return;

resync:
	impl->have_sync = false;
	impl->rtp_last_ts = 0;

	return;
}

static const struct pw_filter_events ptp_sender_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = ptp_sender_destroy,
	.process = ptp_sender_process
};

static int setup_ptp_sender(struct impl *impl, struct pw_core *core, enum pw_direction direction, const char *driver_grp)
{
	const struct spa_pod *params[4];
	struct pw_properties *filter_props = NULL;
	struct spa_pod_builder b;
	uint32_t n_params;
	uint8_t buffer[1024];
	int ret;

	if (direction != PW_DIRECTION_INPUT)
		return 0;

	if (driver_grp == NULL) {
		pw_log_info("AES67 driver group not specified, no separate sender configured");
		return 0;
	}

	pw_log_info("AES67 driver group: %s, setting up separate sender", driver_grp);

	spa_dll_init(&impl->ptp_dll);
	/* BW selected empirically, as it converges most quickly and holds reasonably well in testing */
	spa_dll_set_bw(&impl->ptp_dll, SPA_DLL_BW_MAX, impl->psamples, impl->rate);
	impl->ptp_corr = 1.0;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	filter_props = pw_properties_new(NULL, NULL);
	if (filter_props == NULL) {
		int res = -errno;
		pw_log_error( "can't create properties: %m");
		return res;
	}

	pw_properties_set(filter_props, PW_KEY_NODE_GROUP, driver_grp);
	pw_properties_setf(filter_props, PW_KEY_NODE_NAME, "%s-ptp-sender", pw_stream_get_name(impl->stream));
	pw_properties_set(filter_props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	/*
	 * sess.latency.msec defines how much data is buffered before it is
	 * sent out on the network. This is done by setting the node.latency
	 * to that value, and process function will get chunks of that size.
	 * It is then split up into psamples chunks and send every ptime.
	 *
	 * With this separate sender mechanism we have some latency in stream
	 * via node.latency, and some in ringbuffer between sink and sender.
	 * Ideally we want to have a total latency that still corresponds to
	 * sess.latency.msec. We do this by using the property setting and
	 * splitting some of it as stream latency and some as ringbuffer
	 * latency. The ringbuffer latency is actually determined by how
	 * long we wait before setting `refilling` to false and start the
	 * sending. Also, see `filter_process`.
	 */
	pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_QUANTUM, "%u", impl->psamples);
	pw_properties_setf(filter_props, PW_KEY_NODE_FORCE_RATE, "%u", impl->rate);

	impl->ptp_sender = pw_filter_new(core, NULL, filter_props);
	if (impl->ptp_sender == NULL)
		return -errno;

	pw_filter_add_listener(impl->ptp_sender, &impl->ptp_sender_listener,
			&ptp_sender_events, impl);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info.info.raw);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_Format, &impl->info.info.raw);

	ret = pw_filter_connect(impl->ptp_sender,
			PW_FILTER_FLAG_RT_PROCESS,
			params, n_params);
	if (ret == 0) {
		pw_log_info("created pw_filter for separate sender");
		impl->separate_sender = true;
	} else {
		pw_log_error("failed to create pw_filter for separate sender");
		impl->separate_sender = false;
	}

	return ret;
}

static int rtp_audio_init(struct impl *impl, struct pw_core *core, enum spa_direction direction, const char *ptp_driver)
{
	if (direction == SPA_DIRECTION_INPUT)
		impl->stream_events.process = rtp_audio_process_capture;
	else
		impl->stream_events.process = rtp_audio_process_playback;

	impl->receive_rtp = rtp_audio_receive;
	impl->flush_timeout = rtp_audio_flush_timeout;

	setup_ptp_sender(impl, core, direction, ptp_driver);

	return 0;
}
