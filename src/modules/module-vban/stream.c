/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <sys/socket.h>
#include <arpa/inet.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/layout.h>
#include <spa/param/audio/raw-json.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/debug/log.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-vban/vban.h>
#include <module-vban/stream.h>

#define BUFFER_SIZE			(1u<<22)
#define BUFFER_MASK			(BUFFER_SIZE-1)
#define BUFFER_SIZE2			(BUFFER_SIZE>>1)
#define BUFFER_MASK2			(BUFFER_SIZE2-1)

#define vban_stream_emit(s,m,v,...)		spa_hook_list_call(&s->listener_list, \
							struct vban_stream_events, m, v, ##__VA_ARGS__)
#define vban_stream_emit_destroy(s)		vban_stream_emit(s, destroy, 0)
#define vban_stream_emit_state_changed(s,n,e)	vban_stream_emit(s, state_changed,0,n,e)
#define vban_stream_emit_send_packet(s,i,l)	vban_stream_emit(s, send_packet,0,i,l)
#define vban_stream_emit_send_feedback(s,seq)	vban_stream_emit(s, send_feedback,0,seq)

struct impl {
	struct spa_audio_info info;
	struct spa_audio_info stream_info;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct pw_stream_events stream_events;

	struct spa_hook_list listener_list;
	struct spa_hook listener;

	const struct format_info *format_info;

	void *stream_data;

	uint32_t rate;
	uint32_t stride;
	uint32_t psamples;
	uint32_t mtu;

	struct vban_header header;
	uint32_t timestamp;
	uint32_t n_frames;
	uint32_t samples_per_frame;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];

	struct spa_io_position *io_position;
	struct spa_dll dll;
	double corr;
	uint32_t target_buffer;
	double max_error;

	float last_timestamp;
	float last_time;

	unsigned always_process:1;
	unsigned started:1;
	unsigned have_sync:1;
	unsigned receiving:1;
	unsigned first:1;

	int (*receive_vban)(struct impl *impl, uint8_t *buffer, ssize_t len);
};

#include "module-vban/audio.c"
#include "module-vban/midi.c"

struct format_info {
	uint32_t media_subtype;
	uint32_t format;
	uint32_t size;
	uint8_t format_bit;
};

static const struct format_info audio_format_info[] = {
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_U8, 1, VBAN_DATATYPE_BYTE8, },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S16_LE, 2, VBAN_DATATYPE_INT16, },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S24_LE, 3, VBAN_DATATYPE_INT24, },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S32_LE, 4, VBAN_DATATYPE_INT32, },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_F32_LE, 4, VBAN_DATATYPE_FLOAT32, },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_F64_LE, 8, VBAN_DATATYPE_FLOAT64, },
	{ SPA_MEDIA_SUBTYPE_control, 0, 1, VBAN_SERIAL_MIDI | VBAN_DATATYPE_BYTE8, },
};

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_Position:
		impl->io_position = area;
		break;
	}
}

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static int stream_start(struct impl *impl)
{
	if (impl->started)
		return 0;

	vban_stream_emit_state_changed(impl, true, NULL);

	impl->started = true;
	return 0;
}

static int stream_stop(struct impl *impl)
{
	if (!impl->started)
		return 0;

	vban_stream_emit_state_changed(impl, false, NULL);

	impl->started = false;
	return 0;
}

static void on_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;

	switch (state) {
		case PW_STREAM_STATE_UNCONNECTED:
			pw_log_info("stream disconnected");
			break;
		case PW_STREAM_STATE_ERROR:
			pw_log_error("stream error: %s", error);
			vban_stream_emit_state_changed(impl, false, error);
			break;
		case PW_STREAM_STATE_STREAMING:
			if ((errno = -stream_start(impl)) < 0)
				pw_log_error("failed to start RTP stream: %m");
			break;
		case PW_STREAM_STATE_PAUSED:
			if (!impl->always_process)
				stream_stop(impl);
			impl->have_sync = false;
			break;
		default:
			break;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.io_changed = stream_io_changed,
};

static const struct format_info *find_audio_format_info(const struct spa_audio_info *info)
{
	SPA_FOR_EACH_ELEMENT_VAR(audio_format_info, f)
		if (f->media_subtype == info->media_subtype &&
		    (f->format == 0 || f->format == info->info.raw.format))
			return f;
	return NULL;
}

static int parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	return spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, DEFAULT_FORMAT),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, SPA_STRINGIFY(DEFAULT_RATE)),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION, DEFAULT_POSITION)),
			&props->dict,
			SPA_KEY_AUDIO_FORMAT,
			SPA_KEY_AUDIO_RATE,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_LAYOUT,
			SPA_KEY_AUDIO_POSITION, NULL);
}

static uint32_t msec_to_samples(struct impl *impl, uint32_t msec)
{
	return msec * impl->rate / 1000;
}

static const struct spa_audio_layout_info layouts[] = {
	{ SPA_AUDIO_LAYOUT_Mono },
	{ SPA_AUDIO_LAYOUT_Stereo },
	{ SPA_AUDIO_LAYOUT_2_1 },
	{ SPA_AUDIO_LAYOUT_3_1 },
	{ SPA_AUDIO_LAYOUT_5_0 },
	{ SPA_AUDIO_LAYOUT_5_1 },
	{ SPA_AUDIO_LAYOUT_7_0 },
	{ SPA_AUDIO_LAYOUT_7_1 },
};

static void default_layout(uint32_t channels, uint32_t *position)
{
	SPA_FOR_EACH_ELEMENT_VAR(layouts, l) {
		if (l->n_channels == channels) {
			for (uint32_t i = 0; i < l->n_channels; i++)
				position[i] = l->position[i];
			return;
		}
	}
	for (uint32_t i = 0; i < channels; i++)
		position[i] = SPA_AUDIO_CHANNEL_AUX0 + i;
}

struct vban_stream *vban_stream_new(struct pw_core *core,
		enum pw_direction direction, struct pw_properties *props,
		const struct vban_stream_events *events, void *data)
{
	struct impl *impl;
	const char *str;
	char tmp[64];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	uint32_t n_params, min_samples, max_samples;
	float min_ptime, max_ptime;
	const struct spa_pod *params[1];
	enum pw_stream_flags flags;
	int latency_msec;
	int res;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		res = -errno;
		goto out;
		return NULL;
	}
	impl->first = true;
	spa_hook_list_init(&impl->listener_list);
	impl->stream_events = stream_events;

	if ((str = pw_properties_get(props, "sess.media")) == NULL)
		str = "audio";

	if (spa_streq(str, "audio")) {
		impl->info.media_type = SPA_MEDIA_TYPE_audio;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	}
	else if (spa_streq(str, "midi")) {
		impl->info.media_type = SPA_MEDIA_TYPE_application;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_control;
	}
	else {
		pw_log_error("unsupported media type:%s", str);
		res = -EINVAL;
		goto out;
	}
	memcpy(impl->header.vban, "VBAN", 4);

	switch (impl->info.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		if ((res = parse_audio_info(props, &impl->info.info.raw)) < 0) {
			pw_log_error("can't parse format: %s", spa_strerror(res));
			goto out;
		}
		if (SPA_FLAG_IS_SET(impl->info.info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
			default_layout(impl->info.info.raw.channels, impl->info.info.raw.position);
			SPA_FLAG_CLEAR(impl->info.info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED);
		}
		impl->stream_info = impl->info;
		impl->format_info = find_audio_format_info(&impl->info);
		if (impl->format_info == NULL) {
			pw_log_error("unsupported audio format:%d channels:%d",
					impl->stream_info.info.raw.format,
					impl->stream_info.info.raw.channels);
			res = -EINVAL;
			goto out;
		}
		impl->stride = impl->format_info->size * impl->stream_info.info.raw.channels;
		impl->rate = impl->stream_info.info.raw.rate;

		impl->header.format_SR = vban_sr_index(impl->rate);
		if (impl->header.format_SR == VBAN_SR_MAXNUMBER) {
			pw_log_error("unsupported audio rate:%u", impl->rate);
			res = -EINVAL;
			goto out;
		}
		impl->header.format_bit = impl->format_info->format_bit;
		if ((str = pw_properties_get(props, "sess.name")) == NULL)
			str = "Stream1";
		snprintf(impl->header.stream_name, sizeof(impl->header.stream_name), "%s", str);
		break;
	case SPA_MEDIA_SUBTYPE_control:
		impl->stream_info = impl->info;
		impl->format_info = find_audio_format_info(&impl->info);
		if (impl->format_info == NULL) {
			res = -EINVAL;
			goto out;
		}
		pw_properties_set(props, PW_KEY_FORMAT_DSP, "8 bit raw midi");
		impl->stride = impl->format_info->size;
		impl->rate = pw_properties_get_uint32(props, "midi.rate", 10000);
		if (impl->rate == 0)
			impl->rate = 10000;

		impl->header.format_SR = VBAN_PROTOCOL_SERIAL | VBAN_BPS_115200;
		impl->header.format_nbs = 0;
		impl->header.format_nbc = 0;
		impl->header.format_bit = impl->format_info->format_bit;
		if ((str = pw_properties_get(props, "sess.name")) == NULL)
			str = "Midi1";
		snprintf(impl->header.stream_name, sizeof(impl->header.stream_name), "%s", str);
		break;
	default:
		spa_assert_not_reached();
		break;
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");

	impl->mtu = pw_properties_get_uint32(props, "net.mtu", DEFAULT_MTU);

	str = pw_properties_get(props, "sess.min-ptime");
	if (!spa_atof(str, &min_ptime))
		min_ptime = DEFAULT_MIN_PTIME;
	str = pw_properties_get(props, "sess.max-ptime");
	if (!spa_atof(str, &max_ptime))
		max_ptime = DEFAULT_MAX_PTIME;

	min_samples = (uint32_t)(min_ptime * impl->rate / 1000);
	max_samples = SPA_MIN(256u, (uint32_t)(max_ptime * impl->rate / 1000));

	float ptime = 0.f;
	if ((str = pw_properties_get(props, "vban.ptime")) != NULL)
		if (!spa_atof(str, &ptime))
			ptime = 0.0f;

	if (ptime != 0.f) {
		impl->psamples = (uint32_t)(ptime * impl->rate / 1000);
	} else {
		impl->psamples = impl->mtu / impl->stride;
		impl->psamples = SPA_CLAMP(impl->psamples, min_samples, max_samples);
		if (direction == PW_DIRECTION_OUTPUT)
			pw_properties_set(props, "vban.ptime",
					spa_dtoa(tmp, sizeof(tmp),
						impl->psamples * 1000.0 / impl->rate));
	}
	latency_msec = pw_properties_get_uint32(props,
			"sess.latency.msec", DEFAULT_SESS_LATENCY);
	impl->target_buffer = msec_to_samples(impl, latency_msec);
	impl->max_error = msec_to_samples(impl, ERROR_MSEC);

	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", impl->rate);
	if (direction == PW_DIRECTION_INPUT) {
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
				impl->psamples, impl->rate);
	} else {
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
				impl->target_buffer / 2, impl->rate);
	}

	pw_properties_setf(props, "net.mtu", "%u", impl->mtu);
	pw_properties_setf(props, "vban.rate", "%u", impl->rate);
	if (impl->info.info.raw.channels > 0)
		pw_properties_setf(props, "vban.channels", "%u", impl->info.info.raw.channels);

	spa_dll_init(&impl->dll);
	spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MAX, 128, impl->rate);
	impl->corr = 1.0;

	impl->stream = pw_stream_new(core, "vban-session", props);
	props = NULL;
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto out;
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;


	switch (impl->info.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		params[n_params++] = spa_format_audio_build(&b,
				SPA_PARAM_EnumFormat, &impl->stream_info);
		flags |= PW_STREAM_FLAG_AUTOCONNECT;
		vban_audio_init(impl, direction);
		break;
	case SPA_MEDIA_SUBTYPE_control:
		params[n_params++] = spa_pod_builder_add_object(&b,
                                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                SPA_FORMAT_mediaType,           SPA_POD_Id(SPA_MEDIA_TYPE_application),
                                SPA_FORMAT_mediaSubtype,        SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		vban_midi_init(impl, direction);
		break;
	default:
		res = -EINVAL;
		goto out;
	}

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&impl->stream_events, impl);

	if ((res = pw_stream_connect(impl->stream,
			direction,
			PW_ID_ANY,
			flags,
			params, n_params)) < 0) {
		pw_log_error("can't connect stream: %s", spa_strerror(res));
		goto out;
	}

	if (impl->always_process &&
		(res = stream_start(impl)) < 0)
		goto out;

	spa_hook_list_append(&impl->listener_list, &impl->listener, events, data);

	return (struct vban_stream*)impl;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

void vban_stream_destroy(struct vban_stream *s)
{
	struct impl *impl = (struct impl*)s;

	vban_stream_emit_destroy(impl);

	if (impl->stream)
		pw_stream_destroy(impl->stream);

	spa_hook_list_clean(&impl->listener_list);
	free(impl);
}

int vban_stream_receive_packet(struct vban_stream *s, uint8_t *buffer, size_t len)
{
	struct impl *impl = (struct impl*)s;
	return impl->receive_vban(impl, buffer, len);
}

uint64_t vban_stream_get_time(struct vban_stream *s, uint64_t *rate)
{
	struct impl *impl = (struct impl*)s;
	struct spa_io_position *pos = impl->io_position;

	if (pos == NULL)
		return -EIO;

	*rate = impl->rate;
	return pos->clock.position * impl->rate *
		pos->clock.rate.num / pos->clock.rate.denom;
}
