
#include <spa/utils/endian.h>
#include <spa/utils/overflow.h>
#include <spa/control/ump-utils.h>

#ifdef HAVE_OPUS_CUSTOM
#include <opus/opus.h>
#include <opus/opus_custom.h>
#endif

#define MAX_CHANNELS	SPA_AUDIO_MAX_CHANNELS

struct volume {
	bool mute;
	uint32_t n_volumes;
	float volumes[MAX_CHANNELS];
};

static inline float bswap_f32(float f)
{
	union {
		float f;
		uint32_t u;
	} v;
	v.f = f;
	v.u = bswap_32(v.u);
	return v.f;
}

static inline void do_volume(float *dst, const float *src, struct volume *vol,
		uint32_t ch, uint32_t n_samples, bool recv)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];
	uint32_t i;

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(float));
	else if (v == 1.0f) {
#if __BYTE_ORDER == __BIG_ENDIAN
		for (i = 0; i < n_samples; i++)
			dst[i] = bswap_f32(src[i]);
#else
		memcpy(dst, src, n_samples * sizeof(float));
#endif

	} else {
#if __BYTE_ORDER == __BIG_ENDIAN
		if (recv) {
			for (i = 0; i < n_samples; i++)
				dst[i] = bswap_f32(src[i]) * v;
		} else {
			for (i = 0; i < n_samples; i++)
				dst[i] = bswap_f32(src[i] * v);
		}
#else
		for (i = 0; i < n_samples; i++)
			dst[i] = src[i] * v;
#endif
	}
}

#define ITOF(type,v,scale) \
	(((type)(v)) * (1.0f / (scale)))
#define FTOI(type,v,scale,min,max) \
	(type)(SPA_CLAMPF((v) * (scale), min, max))

#define S16_MIN			-32768
#define S16_MAX			32767
#define S16_SCALE		32768.0f
#define S16_TO_F32(v)		ITOF(int16_t, v, S16_SCALE)
#define F32_TO_S16(v)		FTOI(int16_t, v, S16_SCALE, S16_MIN, S16_MAX)

static inline void do_volume_to_s16(int16_t *dst, const float *src, struct volume *vol,
		uint32_t ch, uint32_t n_samples)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];
	uint32_t i;

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(int16_t));
	else if (v == 1.0f) {
		for (i = 0; i < n_samples; i++)
			dst[i] = F32_TO_S16(src[i]);
	} else {
		for (i = 0; i < n_samples; i++)
			dst[i] = F32_TO_S16(src[i] * v);
	}
}

static inline void do_volume_from_s16(float *dst, const int16_t *src, struct volume *vol,
		uint32_t ch, uint32_t n_samples)
{
	float v = vol->mute ? 0.0f : vol->volumes[ch];
	uint32_t i;

	if (v == 0.0f || src == NULL)
		memset(dst, 0, n_samples * sizeof(float));
	else if (v == 1.0f) {
		for (i = 0; i < n_samples; i++)
			dst[i] = S16_TO_F32(src[i]);
	} else {
		for (i = 0; i < n_samples; i++)
			dst[i] = S16_TO_F32(src[i]) * v;
	}
}

struct netjack2_peer {
	int fd;

	uint32_t our_stream;
	uint32_t other_stream;
	struct nj2_session_params params;
	struct nj2_packet_header sync;
	uint32_t cycle;

	struct volume *send_volume;
	struct volume *recv_volume;

	void *midi_data;
	uint32_t midi_size;

	uint32_t quantum_limit;
	float *empty;
	void *encoded_data;
	uint32_t encoded_size;
	uint32_t max_encoded_size;
#ifdef HAVE_OPUS_CUSTOM
	OpusCustomMode *opus_config;
	OpusCustomEncoder **opus_enc;
	OpusCustomDecoder **opus_dec;
#endif

	unsigned fix_midi:1;
};

static int netjack2_init(struct netjack2_peer *peer)
{
	int res = 0;
	uint32_t max_midi_ch, max_audio_ch;

	if ((peer->empty = calloc(peer->quantum_limit, sizeof(float))) == NULL)
		goto error_errno;

	max_midi_ch = SPA_MAX(peer->params.send_midi_channels, peer->params.recv_midi_channels);
	if (max_midi_ch > MAX_CHANNELS ||
	    spa_overflow_mul(peer->params.period_size, (uint32_t)sizeof(float), &peer->midi_size) ||
	    spa_overflow_mul(peer->midi_size, max_midi_ch, &peer->midi_size)) {
		errno = EINVAL;
		goto error_errno;
	}
	if ((peer->midi_data = calloc(1, peer->midi_size)) == NULL && peer->midi_size > 0)
		goto error_errno;

	max_audio_ch = SPA_MAX(peer->params.send_audio_channels, peer->params.recv_audio_channels);
	if (max_audio_ch > MAX_CHANNELS) {
		errno = EINVAL;
		goto error_errno;
	}

	if (peer->params.sample_encoder == NJ2_ENCODER_INT) {
		if (spa_overflow_mul(peer->params.period_size, (uint32_t)sizeof(int16_t), &peer->max_encoded_size) ||
		    spa_overflow_mul(peer->max_encoded_size, max_audio_ch, &peer->encoded_size)) {
			errno = EINVAL;
			goto error_errno;
		}
		if ((peer->encoded_data = calloc(1, peer->encoded_size)) == NULL)
			goto error_errno;
	} else if (peer->params.sample_encoder == NJ2_ENCODER_OPUS) {
#ifdef HAVE_OPUS_CUSTOM
		int32_t i;
		if (peer->params.sample_rate == 0) {
			errno = EINVAL;
			goto error_errno;
		}
		peer->max_encoded_size = ((uint64_t)peer->params.kbps * peer->params.period_size * 1024) /
			((uint64_t)peer->params.sample_rate * 8) + sizeof(uint16_t);
		if (spa_overflow_mul(peer->max_encoded_size, max_audio_ch, &peer->encoded_size)) {
			errno = EINVAL;
			goto error_errno;
		}
		if ((peer->encoded_data = calloc(1, peer->encoded_size)) == NULL)
			goto error_errno;
		if ((peer->opus_config = opus_custom_mode_create(peer->params.sample_rate,
				peer->params.period_size, &res)) == NULL)
			goto error_opus;
		if ((peer->opus_enc = calloc(peer->params.send_audio_channels,
						sizeof(OpusCustomEncoder*))) == NULL)
			goto error_errno;

		for (i = 0; i < peer->params.send_audio_channels; i++) {
			if ((peer->opus_enc[i] = opus_custom_encoder_create(peer->opus_config,
					1, &res)) == NULL)
				goto error_opus;
			opus_custom_encoder_ctl(peer->opus_enc[i],
				OPUS_SET_BITRATE(peer->params.kbps*1024)); // bits per second
			opus_custom_encoder_ctl(peer->opus_enc[i],
				OPUS_SET_COMPLEXITY(10));
			opus_custom_encoder_ctl(peer->opus_enc[i],
				OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
			opus_custom_encoder_ctl(peer->opus_enc[i],
				OPUS_SET_SIGNAL(OPUS_APPLICATION_RESTRICTED_LOWDELAY));
		}
		if ((peer->opus_dec = calloc(peer->params.recv_audio_channels,
				sizeof(OpusCustomDecoder*))) == NULL)
			goto error_errno;
		for (i = 0; i < peer->params.recv_audio_channels; i++) {
			if ((peer->opus_dec[i] = opus_custom_decoder_create(peer->opus_config,
					1, &res)) == NULL)
				goto error_opus;
		}
#else
		return -ENOTSUP;
#endif

	}
	return res;
error_errno:
	pw_log_warn("error: %m");
	return -errno;
#ifdef HAVE_OPUS_CUSTOM
error_opus:
	pw_log_warn("error: %d", res);
	return -EINVAL;
#endif
}

static void netjack2_cleanup(struct netjack2_peer *peer)
{

	free(peer->empty);
	free(peer->midi_data);
#ifdef HAVE_OPUS_CUSTOM
	int32_t i;
	if (peer->opus_enc != NULL) {
		for (i = 0; i < peer->params.send_audio_channels; i++) {
			if (peer->opus_enc[i])
				opus_custom_encoder_destroy(peer->opus_enc[i]);
		}
		free(peer->opus_enc);
	}
	if (peer->opus_dec != NULL) {
		for (i = 0; i < peer->params.recv_audio_channels; i++) {
			if (peer->opus_dec[i])
				opus_custom_decoder_destroy(peer->opus_dec[i]);
		}
		free(peer->opus_dec);
	}
	if (peer->opus_config)
		opus_custom_mode_destroy(peer->opus_config);
	free(peer->encoded_data);
#endif
	spa_zero(*peer);
}

struct data_info {
	uint32_t id;
	void *data;
	bool filled;
};

static inline void fix_midi_event(uint8_t *data, size_t size)
{
	/* fixup NoteOn with vel 0 */
	if (size > 2 && (data[0] & 0xF0) == 0x90 && data[2] == 0x00) {
		data[0] = 0x80 + (data[0] & 0x0F);
		data[2] = 0x40;
	}
}

static inline void *n2j_midi_buffer_reserve(struct nj2_midi_buffer *buf,
		uint32_t offset, uint32_t size)
{
	struct nj2_midi_event *ev;
	void *ptr;

	if (size <= 0)
		return NULL;

	size_t used_size = sizeof(*buf) + buf->write_pos +
		((buf->event_count + 1) * sizeof(struct nj2_midi_event));

	ev = &buf->event[buf->event_count];
	ev->time = offset;
	ev->size = size;
	if (size <= MIDI_INLINE_MAX) {
		ptr = ev->buffer;
	} else {
		if (used_size + size > buf->buffer_size)
			return NULL;
		buf->write_pos += size;
		ev->offset = buf->buffer_size - buf->write_pos;
		ptr = SPA_PTROFF(buf, ev->offset, void);
	}
	buf->event_count++;
	return ptr;
}

static inline void n2j_midi_buffer_write(struct nj2_midi_buffer *buf,
		uint32_t offset, const void *data, uint32_t size, bool fix)
{
	uint8_t *ptr = n2j_midi_buffer_reserve(buf, offset, size);
	if (ptr != NULL) {
		memcpy(ptr, data, size);
		if (fix)
			fix_midi_event(ptr, size);
	}
	else
		buf->lost_events++;
}

static void midi_to_netjack2(struct netjack2_peer *peer,
		struct nj2_midi_buffer *buf, float *src, uint32_t n_samples)
{
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct spa_pod_sequence seq;
	struct spa_pod_control c;
	const void *seq_body, *c_body;

	buf->magic = MIDI_BUFFER_MAGIC;
	buf->buffer_size = peer->params.period_size * sizeof(float);
	buf->nframes = n_samples;
	buf->write_pos = 0;
	buf->event_count = 0;
	buf->lost_events = 0;

	if (src == NULL)
		return;

	spa_pod_parser_init_from_data(&parser, src, n_samples * sizeof(float),
			0, n_samples * sizeof(float));
	if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) < 0)
		return;

	while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
		uint32_t size = c.value.size;
		const uint8_t *data = c_body;

		if (c.type != SPA_CONTROL_Midi)
			continue;

		if (c.offset >= n_samples) {
			buf->lost_events++;
			continue;
		}
		n2j_midi_buffer_write(buf, c.offset, data, size, peer->fix_midi);
	}
	if (buf->write_pos > 0)
		memmove(SPA_PTROFF(buf, sizeof(*buf) + buf->event_count * sizeof(struct nj2_midi_event), void),
			SPA_PTROFF(buf, buf->buffer_size - buf->write_pos, void),
			buf->write_pos);
}

static inline void netjack2_clear_midi(float *dst, uint32_t size)
{
	struct spa_pod_builder b = { 0, };
	struct spa_pod_frame f;
	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);
	spa_pod_builder_pop(&b, &f);
}

static inline void netjack2_to_midi(float *dst, uint32_t size, struct nj2_midi_buffer *buf)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i;
	struct spa_pod_frame f;
	size_t used = sizeof(*buf) +
			(buf->event_count * sizeof(struct nj2_midi_event)) +
			buf->write_pos;

	spa_pod_builder_init(&b, dst, size);
	spa_pod_builder_push_sequence(&b, &f, 0);

	if (used > size)
		goto done;

	size_t offset = size - used;

	for (i = 0; i < buf->event_count; i++) {
		struct nj2_midi_event *ev = &buf->event[i];
		uint8_t *data;

		if (ev->size <= MIDI_INLINE_MAX) {
			data = ev->buffer;
		} else if (ev->offset > offset &&
			   ev->offset - offset + ev->size <= used) {
			data = SPA_PTROFF(buf, ev->offset - offset, void);
		} else {
			continue;
		}

		spa_pod_builder_control(&b, ev->time, SPA_CONTROL_Midi);
                spa_pod_builder_bytes(&b, data, ev->size);
	}
done:
	spa_pod_builder_pop(&b, &f);
}

static int netjack2_send_sync(struct netjack2_peer *peer, uint32_t nframes)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu];
	uint32_t i, packet_size, active_ports, is_last;
	int32_t *p;

	/* we always listen on all ports */
	active_ports = peer->params.recv_audio_channels;
	packet_size = sizeof(header) + active_ports * sizeof(int32_t);
	is_last = peer->params.send_midi_channels == 0 &&
                        peer->params.send_audio_channels == 0 ? 1 : 0;

	strncpy(header.type, "header", sizeof(header.type));
	header.data_type = htonl('s');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.num_packets = 0;
	header.packet_size = htonl(packet_size);
	header.active_ports = htonl(active_ports);
	header.cycle = htonl(peer->cycle);
	header.sub_cycle = 0;
	header.frames = htonl(nframes);
	header.is_last = htonl(is_last);

	memcpy(buffer, &header, sizeof(header));
	p = SPA_PTROFF(buffer, sizeof(header), int32_t);
	for (i = 0; i < active_ports; i++)
		p[i] = htonl(i);
	send(peer->fd, buffer, packet_size, 0);
	return 0;
}

static int netjack2_send_midi(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu], *midi_data;
	uint32_t i, num_packets, active_ports, midi_size;
	uint32_t max_size;

	active_ports = peer->params.send_midi_channels;
	if (active_ports <= 0)
		return 0;

	midi_size = 0;
	midi_data = peer->midi_data;

	for (i = 0; i < active_ports; i++) {
		struct nj2_midi_buffer *mbuf;
		void *data = (i < n_info && info) ? info[i].data : NULL;

		mbuf = SPA_PTROFF(midi_data, midi_size, struct nj2_midi_buffer);
		midi_to_netjack2(peer, mbuf, data, nframes);

		midi_size += sizeof(*mbuf)
			+ mbuf->event_count * sizeof(struct nj2_midi_event)
			+ mbuf->write_pos;

		nj2_midi_buffer_hton(mbuf, mbuf);
	}

	/* Note: jack2 calculates the packet max_size and num packets with
	 * different values... */
	max_size = peer->params.mtu - sizeof(header);
	num_packets = (midi_size + max_size-1) / max_size;

	strncpy(header.type, "header", sizeof(header.type));
	header.data_type = htonl('m');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = ((i == num_packets - 1) && peer->params.send_audio_channels == 0) ? 1 : 0;
		uint32_t size = midi_size - i * max_size;
		uint32_t copy_size = SPA_MIN(size, max_size);
		uint32_t packet_size = sizeof(header) + copy_size;

		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
                header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		memcpy(SPA_PTROFF(buffer, sizeof(header), void),
			SPA_PTROFF(midi_data, i * max_size, void),
			copy_size);
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
}

static int netjack2_send_float(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu];
	uint32_t i, j, active_ports, num_packets;
	uint32_t sub_period_size, sub_period_bytes;

	if (peer->params.send_audio_channels <= 0)
		return 0;

	active_ports = n_info;

	if (active_ports == 0) {
		sub_period_size = nframes;
	} else {
		uint32_t max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
		uint32_t overhead = active_ports * sizeof(int32_t);
		if (max_size <= overhead) {
			sub_period_size = 1;
		} else {
			uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)(max_size - overhead) /
					(active_ports * sizeof(float))) / logf(2.f)));
			sub_period_size = SPA_CLAMP(period, 1u, nframes);
		}
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);
	num_packets = nframes / sub_period_size;

	strncpy(header.type, "header", sizeof(header.type));
	header.data_type = htonl('a');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = (i == num_packets - 1) ? 1 : 0;
		uint32_t packet_size = sizeof(header) + active_ports * sub_period_bytes;
		int32_t *ap = SPA_PTROFF(buffer, sizeof(header), int32_t);
		float *src;

		for (j = 0; j < active_ports; j++) {
			ap[0] = htonl(info[j].id);

			src = SPA_PTROFF(info[j].data, i * sub_period_size * sizeof(float), float);
			do_volume((float*)&ap[1], src, peer->send_volume, info[j].id, sub_period_size, false);

			ap = SPA_PTROFF(ap, sub_period_bytes, int32_t);
		}
		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
		header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
}

static int netjack2_send_opus(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
#ifdef HAVE_OPUS_CUSTOM
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu], *encoded_data;
	uint32_t i, j, active_ports, num_packets, max_size, max_encoded;
	uint32_t sub_period_bytes, last_period_bytes;

	active_ports = peer->params.send_audio_channels;
	if (active_ports <= 0)
		return 0;

	max_encoded = peer->max_encoded_size;

	max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
	num_packets = ((active_ports * max_encoded) + max_size-1) / max_size;

	sub_period_bytes = max_encoded / num_packets;
	last_period_bytes = sub_period_bytes + max_encoded % num_packets;

	encoded_data = peer->encoded_data;

	for (i = 0; i < active_ports; i++) {
		uint16_t *ap = SPA_PTROFF(encoded_data, i * max_encoded, uint16_t);
		void *pcm;
		int res;

		if (i >= n_info || (pcm = info[i].data) == NULL)
			pcm = peer->empty;

		res = opus_custom_encode_float(peer->opus_enc[i],
				pcm, nframes, (unsigned char*)&ap[1], max_encoded - 2);

		if (res < 0 || res > 0xffff) {
			pw_log_warn("encoding error %d", res);
			ap[0] = 0;
		} else {
			ap[0] = htons(res);
		}
	}

	strncpy(header.type, "header", sizeof(header.type));
	header.data_type = htonl('a');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = (i == num_packets - 1) ? 1 : 0;
		uint32_t data_size, packet_size;

		data_size = is_last ? last_period_bytes : sub_period_bytes;
		packet_size = sizeof(header) + active_ports * data_size;

		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
		header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		for (j = 0; j < active_ports; j++) {
			memcpy(SPA_PTROFF(buffer, sizeof(header) + j * data_size, void),
					SPA_PTROFF(encoded_data,
						j * max_encoded + i * sub_period_bytes, void),
					data_size);
		}
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
#else
	return -ENOTSUP;
#endif
}


static int netjack2_send_int(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *info, uint32_t n_info)
{
	struct nj2_packet_header header;
	uint8_t buffer[peer->params.mtu], *encoded_data;
	uint32_t i, j, active_ports, num_packets, max_size, max_encoded;
	uint32_t sub_period_bytes, last_period_bytes;

	active_ports = peer->params.send_audio_channels;
	if (active_ports <= 0)
		return 0;

	max_encoded = peer->max_encoded_size;

	max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
	num_packets = ((active_ports * max_encoded) + max_size-1) / max_size;

	sub_period_bytes = max_encoded / num_packets;
	last_period_bytes = sub_period_bytes + max_encoded % num_packets;

	encoded_data = peer->encoded_data;

	for (i = 0; i < active_ports; i++) {
		int16_t *ap = SPA_PTROFF(encoded_data, i * max_encoded, int16_t);
		void *pcm;

		if (i < n_info && (pcm = info[i].data) != NULL)
			do_volume_to_s16(ap, pcm, peer->send_volume, i, nframes);
		else
			memset(ap, 0, max_encoded);
	}

	strncpy(header.type, "header", sizeof(header.type));
	header.data_type = htonl('a');
	header.data_stream = htonl(peer->our_stream);
	header.id = htonl(peer->params.id);
	header.cycle = htonl(peer->cycle);
	header.active_ports = htonl(active_ports);
	header.num_packets = htonl(num_packets);
	header.frames = htonl(nframes);

	for (i = 0; i < num_packets; i++) {
		uint32_t is_last = (i == num_packets - 1) ? 1 : 0;
		uint32_t data_size, packet_size;

		data_size = is_last ? last_period_bytes : sub_period_bytes;
		packet_size = sizeof(header) + active_ports * data_size;

		header.sub_cycle = htonl(i);
		header.is_last = htonl(is_last);
		header.packet_size = htonl(packet_size);
		memcpy(buffer, &header, sizeof(header));
		for (j = 0; j < active_ports; j++) {
			memcpy(SPA_PTROFF(buffer, sizeof(header) + j * data_size, void),
					SPA_PTROFF(encoded_data,
						j * max_encoded + i * sub_period_bytes, void),
					data_size);
		}
		send(peer->fd, buffer, packet_size, 0);
		//nj2_dump_packet_header(&header);
	}
	return 0;
}

static int netjack2_send_data(struct netjack2_peer *peer, uint32_t nframes,
		struct data_info *midi, uint32_t n_midi,
		struct data_info *audio, uint32_t n_audio)
{
	netjack2_send_sync(peer, nframes);
	netjack2_send_midi(peer, nframes, midi, n_midi);
	switch (peer->params.sample_encoder) {
	case NJ2_ENCODER_INT:
		netjack2_send_int(peer, nframes, audio, n_audio);
		break;
	case NJ2_ENCODER_FLOAT:
		netjack2_send_float(peer, nframes, audio, n_audio);
		break;
	case NJ2_ENCODER_OPUS:
		netjack2_send_opus(peer, nframes, audio, n_audio);
		break;
	}
	return 0;
}

#define MAX_RECV_PACKETS 1024

static inline int32_t netjack2_driver_sync_wait(struct netjack2_peer *peer)
{
	struct nj2_packet_header sync;
	ssize_t len;
	uint32_t tries = 0;

	while (true) {
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;

		if (len >= (ssize_t)sizeof(sync)) {
			//nj2_dump_packet_header(&sync);

			if (strncmp(sync.type, "header", sizeof(sync.type)) == 0 &&
			    ntohl(sync.data_type) == 's' &&
			    ntohl(sync.data_stream) == peer->other_stream &&
			    ntohl(sync.id) == peer->params.id)
				break;
		}
		if (++tries > MAX_RECV_PACKETS) {
			pw_log_warn("too many packets in sync wait, aborting");
			return -ENOENT;
		}
	}
	peer->sync.is_last = ntohl(sync.is_last);
	peer->sync.frames = ntohl(sync.frames);
	if (peer->sync.frames == -1)
		peer->sync.frames = peer->params.period_size;
	peer->sync.frames = SPA_MIN(peer->sync.frames, (int32_t)peer->quantum_limit);

	return peer->sync.frames;

receive_error:
	pw_log_warn("recv error: %m");
	return -errno;
}

static inline int32_t netjack2_manager_sync_wait(struct netjack2_peer *peer)
{
	struct nj2_packet_header sync;
	ssize_t len;
	int32_t offset;
	uint32_t tries = 0;

	while (true) {
		if ((len = recv(peer->fd, &sync, sizeof(sync), MSG_PEEK)) < 0)
			goto receive_error;

		if (len >= (ssize_t)sizeof(sync)) {
			//nj2_dump_packet_header(sync);

			if (strncmp(sync.type, "header", sizeof(sync.type)) == 0 &&
			    ntohl(sync.data_type) == 's' &&
			    ntohl(sync.data_stream) == peer->other_stream &&
			    ntohl(sync.id) == peer->params.id)
				break;
		}
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;
		if (++tries > MAX_RECV_PACKETS) {
			pw_log_warn("too many packets in sync wait, aborting");
			return -ENOENT;
		}
	}
	peer->sync.cycle = ntohl(sync.cycle);
	peer->sync.is_last = ntohl(sync.is_last);
	peer->sync.frames = ntohl(sync.frames);
	if (peer->sync.frames == -1)
		peer->sync.frames = peer->params.period_size;
	peer->sync.frames = SPA_MIN(peer->sync.frames, (int32_t)peer->quantum_limit);

	offset = peer->cycle - peer->sync.cycle;
	if (offset < (int32_t)peer->params.network_latency) {
		pw_log_info("sync offset %d %d %d", peer->cycle, peer->sync.cycle, offset);
		peer->sync.is_last = true;
		return 0;
	} else {
		if ((len = recv(peer->fd, &sync, sizeof(sync), 0)) < 0)
			goto receive_error;
	}
	return peer->sync.frames;

receive_error:
	pw_log_warn("recv error: %m");
	return -errno;
}

static int netjack2_recv_midi(struct netjack2_peer *peer, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	ssize_t len;
	uint32_t i, active_ports, sub_cycle, max_size, offset, midi_size;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size], *data = buffer, *midi_data;

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;
	if ((size_t)len < sizeof(*header))
		return -EINVAL;

	active_ports = peer->params.recv_midi_channels;
	if (active_ports == 0)
		return 0;

	sub_cycle = ntohl(header->sub_cycle);
	peer->sync.num_packets = ntohl(header->num_packets);
	max_size = peer->params.mtu - sizeof(*header);

	if (spa_overflow_mul(max_size, sub_cycle, &offset))
		return -EOVERFLOW;

	data += sizeof(*header);
	len -= sizeof(*header);

	midi_data = peer->midi_data;
	midi_size = peer->midi_size;

	if (offset <= midi_size && (size_t)len <= midi_size - offset)
		memcpy(SPA_PTROFF(midi_data, offset, void), data, len);

	if (++(*count) < peer->sync.num_packets)
		return 0;

	for (i = 0; i < active_ports; i++) {
		struct nj2_midi_buffer *mbuf = (struct nj2_midi_buffer *)midi_data;
		size_t used, events_size;

		nj2_midi_buffer_ntoh(mbuf, mbuf);

		if (spa_overflow_mul((size_t)mbuf->event_count, sizeof(struct nj2_midi_event), &events_size) ||
		    spa_overflow_add(events_size, (size_t)mbuf->write_pos, &used) ||
		    spa_overflow_add(used, sizeof(*mbuf), &used) ||
		    used > midi_size)
			break;

		if (i < n_info && info[i].data != NULL) {
			netjack2_to_midi(info[i].data, peer->params.period_size * sizeof(float), mbuf);
			info[i].filled = true;
		}
		midi_data += used;
		midi_size -= used;
	}
	return 0;
}

static int netjack2_recv_float(struct netjack2_peer *peer, struct nj2_packet_header *header, uint32_t *count,
		struct data_info *info, uint32_t n_info)
{
	ssize_t len;
	uint32_t i, sub_cycle, sub_period_size, sub_period_bytes, active_ports;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size];

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;

	active_ports = ntohl(header->active_ports);
	if (active_ports == 0 || active_ports > MAX_CHANNELS)
		return 0;

	uint32_t nframes = peer->sync.frames;
	uint32_t max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
	uint32_t overhead = active_ports * sizeof(int32_t);
	if (max_size <= overhead) {
		sub_period_size = 1;
	} else {
		uint32_t period = (uint32_t) powf(2.f, (uint32_t) (logf((float)(max_size - overhead) /
				(active_ports * sizeof(float))) / logf(2.f)));
		sub_period_size = SPA_CLAMP(period, 1u, nframes);
	}
	sub_period_bytes = sub_period_size * sizeof(float) + sizeof(int32_t);

	if ((size_t)len < (size_t)active_ports * sub_period_bytes + sizeof(*header))
		return 0;

	sub_cycle = ntohl(header->sub_cycle);
	if (sub_cycle >= nframes / sub_period_size)
		return 0;

	for (i = 0; i < active_ports; i++) {
		int32_t *ap = SPA_PTROFF(buffer, sizeof(*header) + i * sub_period_bytes, int32_t);
		uint32_t active_port = ntohl(ap[0]);
		void *data;

		pw_log_trace_fp("%u/%u %u %u", active_port, n_info,
				sub_cycle, sub_period_size);
		if (active_port >= n_info)
			continue;

		if ((data = info[active_port].data) != NULL) {
			float *dst = SPA_PTROFF(data,
					sub_cycle * sub_period_size * sizeof(float),
					float);
			do_volume(dst, (float*)&ap[1], peer->recv_volume, active_port, sub_period_size, true);
			info[active_port].filled = true;
		}
	}
	return 0;
}

static int netjack2_recv_opus(struct netjack2_peer *peer, struct nj2_packet_header *header,
		uint32_t *count, struct data_info *info, uint32_t n_info)
{
#ifdef HAVE_OPUS_CUSTOM
	ssize_t len;
	uint32_t i, active_ports, sub_cycle, max_size, encoded_size, max_encoded;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size], *data = buffer, *encoded_data;
	uint32_t sub_period_bytes, last_period_bytes, data_size, num_packets;

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;
	if ((size_t)len < sizeof(*header))
		return -EINVAL;

	active_ports = peer->params.recv_audio_channels;
	if (active_ports == 0)
		return 0;

	sub_cycle = ntohl(header->sub_cycle);
	peer->sync.num_packets = ntohl(header->num_packets);

	max_encoded = peer->max_encoded_size;

	max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
	num_packets = ((active_ports * max_encoded) + max_size-1) / max_size;

	sub_period_bytes = max_encoded / num_packets;
	last_period_bytes = sub_period_bytes + max_encoded % num_packets;

	data += sizeof(*header);
	len -= sizeof(*header);

	if (sub_cycle == peer->sync.num_packets-1)
		data_size = last_period_bytes;
	else
		data_size = sub_period_bytes;

	encoded_data = peer->encoded_data;
	encoded_size = peer->encoded_size;

	uint32_t end_offset, cycle_offset;
	if (spa_overflow_mul(sub_cycle, sub_period_bytes, &cycle_offset) ||
	    spa_overflow_add(cycle_offset, (active_ports - 1) * max_encoded, &end_offset) ||
	    spa_overflow_add(end_offset, data_size, &end_offset) ||
	    end_offset > encoded_size)
		return -ENOSPC;

	if (spa_overflow_mul(active_ports, data_size, &end_offset) ||
	    end_offset > (uint32_t)len)
		return -EINVAL;

	for (i = 0; i < active_ports; i++) {
		memcpy(SPA_PTROFF(encoded_data,
				i * max_encoded + cycle_offset, void),
				SPA_PTROFF(data, i * data_size, void),
				data_size);
	}
	if (++(*count) < peer->sync.num_packets)
		return 0;

	for (i = 0; i < active_ports; i++) {
		uint16_t *ap = SPA_PTROFF(encoded_data, i * max_encoded, uint16_t);
		uint16_t encoded_len = ntohs(ap[0]);
		void *pcm;
		int res;

		if (i >= n_info || (pcm = info[i].data) == NULL)
			continue;

		if (encoded_len > max_encoded - sizeof(uint16_t))
			continue;

		res = opus_custom_decode_float(peer->opus_dec[i],
				(unsigned char*)&ap[1], encoded_len,
				pcm, peer->sync.frames);

		if (res < 0 || res > 0xffff || res != peer->sync.frames)
			pw_log_warn("decoding error %d", res);
		else
			info[i].filled = true;
	}
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int netjack2_recv_int(struct netjack2_peer *peer, struct nj2_packet_header *header,
		uint32_t *count, struct data_info *info, uint32_t n_info)
{
	ssize_t len;
	uint32_t i, active_ports, sub_cycle, max_size, encoded_size, max_encoded;
	uint32_t packet_size = SPA_MIN(ntohl(header->packet_size), peer->params.mtu);
	uint8_t buffer[packet_size], *data = buffer, *encoded_data;
	uint32_t sub_period_bytes, last_period_bytes, data_size, num_packets;

	if ((len = recv(peer->fd, buffer, packet_size, 0)) < 0)
		return -errno;
	if ((size_t)len < sizeof(*header))
		return -EINVAL;

	active_ports = peer->params.recv_audio_channels;
	if (active_ports == 0)
		return 0;

	sub_cycle = ntohl(header->sub_cycle);
	peer->sync.num_packets = ntohl(header->num_packets);

	max_encoded = peer->max_encoded_size;

	max_size = PACKET_AVAILABLE_SIZE(peer->params.mtu);
	num_packets = ((active_ports * max_encoded) + max_size-1) / max_size;

	sub_period_bytes = max_encoded / num_packets;
	last_period_bytes = sub_period_bytes + max_encoded % num_packets;

	data += sizeof(*header);
	len -= sizeof(*header);

	if (sub_cycle == peer->sync.num_packets-1)
		data_size = last_period_bytes;
	else
		data_size = sub_period_bytes;

	encoded_data = peer->encoded_data;
	encoded_size = peer->encoded_size;

	uint32_t end_offset, cycle_offset;
	if (spa_overflow_mul(sub_cycle, sub_period_bytes, &cycle_offset) ||
	    spa_overflow_add(cycle_offset, (active_ports - 1) * max_encoded, &end_offset) ||
	    spa_overflow_add(end_offset, data_size, &end_offset) ||
	    end_offset > encoded_size)
		return -ENOSPC;

	if (spa_overflow_mul(active_ports, data_size, &end_offset) ||
	    end_offset > (uint32_t)len)
		return -EINVAL;

	for (i = 0; i < active_ports; i++) {
		memcpy(SPA_PTROFF(encoded_data,
				i * max_encoded + cycle_offset, void),
				SPA_PTROFF(data, i * data_size, void),
				data_size);
	}
	if (++(*count) < peer->sync.num_packets)
		return 0;

	for (i = 0; i < active_ports; i++) {
		int16_t *ap = SPA_PTROFF(encoded_data, i * max_encoded, int16_t);
		void *pcm;

		if (i >= n_info || (pcm = info[i].data) == NULL)
			continue;

		do_volume_from_s16(pcm, ap, peer->recv_volume, i, peer->sync.frames);
		info[i].filled = true;
	}
	return 0;
}

static int netjack2_recv_data(struct netjack2_peer *peer,
		struct data_info *midi, uint32_t n_midi,
		struct data_info *audio, uint32_t n_audio)
{
	ssize_t len;
	uint32_t i, audio_count = 0, midi_count = 0, packet_count = 0;
	struct nj2_packet_header header;

	while (!peer->sync.is_last) {
		if (++packet_count > MAX_RECV_PACKETS) {
			pw_log_warn("too many packets in cycle (%u), aborting",
					MAX_RECV_PACKETS);
			break;
		}
		if ((len = recv(peer->fd, &header, sizeof(header), MSG_PEEK)) < 0)
			goto receive_error;

		if (len < (ssize_t)sizeof(header))
			goto receive_error;

		//nj2_dump_packet_header(&header);

		if (ntohl(header.data_stream) != peer->other_stream ||
		    ntohl(header.id) != peer->params.id) {
			uint8_t discard[peer->params.mtu];
			pw_log_debug("not our packet, discarding");
			recv(peer->fd, discard, sizeof(discard), 0);
			continue;
		}

		peer->sync.is_last = ntohl(header.is_last);

		switch (ntohl(header.data_type)) {
		case 'm':
			netjack2_recv_midi(peer, &header, &midi_count, midi, n_midi);
			break;
		case 'a':
			switch (peer->params.sample_encoder) {
			case NJ2_ENCODER_FLOAT:
				netjack2_recv_float(peer, &header, &audio_count, audio, n_audio);
				break;
			case NJ2_ENCODER_OPUS:
				netjack2_recv_opus(peer, &header, &audio_count, audio, n_audio);
				break;
			case NJ2_ENCODER_INT:
				netjack2_recv_int(peer, &header, &audio_count, audio, n_audio);
				break;
			}
			break;
		case 's':
			pw_log_info("missing last data packet");
			peer->sync.is_last = true;
			break;
		}
	}
	for (i = 0; i < n_audio; i++) {
		if (!audio[i].filled && audio[i].data != NULL)
			memset(audio[i].data, 0, peer->sync.frames * sizeof(float));
	}
	for (i = 0; i < n_midi; i++) {
		if (!midi[i].filled && midi[i].data != NULL)
			netjack2_clear_midi(midi[i].data, peer->params.period_size * sizeof(float));
	}
	peer->sync.cycle = ntohl(header.cycle);
	return 0;

receive_error:
	pw_log_warn("recv error: %m");
	return -errno;
}
