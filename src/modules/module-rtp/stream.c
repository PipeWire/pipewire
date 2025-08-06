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
#include <spa/param/audio/raw-json.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/debug/log.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/rtp.h>
#include <module-rtp/stream.h>
#include <module-rtp/apple-midi.h>

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define BUFFER_SIZE			(1u<<22)
#define BUFFER_MASK			(BUFFER_SIZE-1)
#define BUFFER_SIZE2			(BUFFER_SIZE>>1)
#define BUFFER_MASK2			(BUFFER_SIZE2-1)

#define rtp_stream_emit(s,m,v,...)		spa_hook_list_call(&s->listener_list, \
							struct rtp_stream_events, m, v, ##__VA_ARGS__)
#define rtp_stream_emit_destroy(s)		rtp_stream_emit(s, destroy, 0)
#define rtp_stream_emit_state_changed(s,n,e)	rtp_stream_emit(s, state_changed,0,n,e)
#define rtp_stream_emit_param_changed(s,i,p)	rtp_stream_emit(s, param_changed,0,i,p)
#define rtp_stream_emit_send_packet(s,i,l)	rtp_stream_emit(s, send_packet,0,i,l)
#define rtp_stream_emit_send_feedback(s,seq)	rtp_stream_emit(s, send_feedback,0,seq)

struct impl {
	struct spa_audio_info info;
	struct spa_audio_info stream_info;

	struct pw_context *context;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct pw_stream_events stream_events;

	struct spa_hook_list listener_list;
	struct spa_hook listener;

	const struct format_info *format_info;

	enum spa_direction direction;
	void *stream_data;

	uint32_t rate;
	uint32_t stride;
	uint32_t actual_max_buffer_size;
	uint8_t payload;
	uint32_t ssrc;
	uint16_t seq;
	unsigned fixed_ssrc:1;
	unsigned have_ssrc:1;
	unsigned ignore_ssrc:1;
	unsigned have_seq:1;
	unsigned marker_on_first:1;
	uint32_t ts_offset;
	uint32_t psamples;
	uint32_t mtu;
	uint32_t header_size;
	uint32_t payload_size;

	struct spa_ringbuffer ring;
	uint8_t buffer[BUFFER_SIZE];
	uint64_t last_recv_timestamp;

	struct spa_io_rate_match *io_rate_match;
	struct spa_io_position *io_position;
	struct spa_dll dll;
	double corr;
	uint32_t target_buffer;
	double max_error;

	float last_timestamp;
	float last_time;

	unsigned direct_timestamp:1;
	unsigned always_process:1;
	unsigned started:1;
	unsigned have_sync:1;
	unsigned receiving:1;
	unsigned first:1;

	struct pw_loop *main_loop;
	struct pw_loop *data_loop;
	struct spa_source *timer;
	bool timer_running;

	int (*receive_rtp)(struct impl *impl, uint8_t *buffer, ssize_t len);
	/* Used for resetting the ring buffer before the stream starts, to prevent
	 * reading from uninitialized memory. This can otherwise happen in direct
	 * timestamp mode when the read index is set to an uninitialized location.
	 * This is a function pointer to allow customizations in case resetting
	 * requires filling the ring buffer with something other than nullbytes
	 * (this can happen with DSD for example). */
	void (*reset_ringbuffer)(struct impl *impl);
	void (*stop_timer)(struct impl *impl);
	void (*flush_timeout)(struct impl *impl, uint64_t expirations);
	void (*deinit)(struct impl *impl, enum spa_direction direction);

	/*
	 * pw_filter where the filter would be driven at the PTP clock
	 * rate with RTP sink being driven at the sink driver clock rate
	 * or some ALSA clock rate.
	 */
	struct pw_filter *ptp_sender;
	struct spa_hook ptp_sender_listener;
	struct spa_dll ptp_dll;
	double ptp_corr;
	bool separate_sender;
	bool refilling;

	/* Track some variables we need from the sink driver */
	uint64_t sink_next_nsec;
	uint64_t sink_nsec;
	uint64_t sink_resamp_delay;
	uint64_t sink_quantum;
	/* And some bookkeping for the sender processing */
	uint64_t rtp_base_ts;
	uint32_t rtp_last_ts;
};

static int do_emit_state_changed(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	bool *started = (bool *)data;

	rtp_stream_emit_state_changed(impl, *started, NULL);
	return 0;
}

#include "module-rtp/audio.c"
#include "module-rtp/midi.c"
#include "module-rtp/opus.c"

struct format_info {
	uint32_t media_subtype;
	uint32_t format;
	uint32_t size;
	const char *mime;
	const char *media_type;
};

static const struct format_info audio_format_info[] = {
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_U8, 1, "L8", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ALAW, 1, "PCMA", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ULAW, 1, "PCMU", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S16_BE, 2, "L16", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S16_LE, 2, "L16", "audio" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S24_BE, 3, "L24", "audio" },
	{ SPA_MEDIA_SUBTYPE_control, 0, 1, "rtp-midi", "audio" },
	{ SPA_MEDIA_SUBTYPE_opus, 0, 4, "opus", "audio" },
};

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_RateMatch:
		impl->io_rate_match = area;
		break;
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

	impl->first = true;

	/* Stop the timer now (if the timer is used). Any lingering timer
	 * will try to send data that is stale at this point, especially
	 * after the ring buffer contents get reset. Worse, the timer might
	 * emit a "stopped" state change after a "started" state change
	 * is emitted here, causing undefined behavior. */
	if (impl->stop_timer)
		impl->stop_timer(impl);

	impl->reset_ringbuffer(impl);

	rtp_stream_emit_state_changed(impl, true, NULL);

	if (impl->separate_sender) {
		struct spa_dict_item items[1];
		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_ALWAYS_PROCESS, "true");

		pw_filter_set_active(impl->ptp_sender, true);
		pw_filter_update_properties(impl->ptp_sender, NULL, &SPA_DICT_INIT(items, 1));

		pw_log_info("activated pw_filter for separate sender");
	}

	impl->started = true;

	return 0;
}

static int stream_stop(struct impl *impl)
{
	if (!impl->started)
		return 0;

	/* if timer is running, the state changed event must be emitted by the timer after all packets have been sent */
	if (!impl->timer_running)
		rtp_stream_emit_state_changed(impl, false, NULL);

	if (impl->separate_sender) {
		struct spa_dict_item items[1];
		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_ALWAYS_PROCESS, "false");

		pw_filter_update_properties(impl->ptp_sender, NULL, &SPA_DICT_INIT(items, 1));

		pw_log_info("deactivating pw_filter for separate sender");
		pw_filter_set_active(impl->ptp_sender, false);
	}

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

static void on_stream_param_changed (void *d, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = d;
	rtp_stream_emit_param_changed(impl, id, param);
};

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
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

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, DEFAULT_FORMAT),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, SPA_STRINGIFY(DEFAULT_RATE)),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION, DEFAULT_POSITION)),
			&props->dict,
			SPA_KEY_AUDIO_FORMAT,
			SPA_KEY_AUDIO_RATE,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL);
}

static uint32_t msec_to_samples(struct impl *impl, float msec)
{
	return (uint32_t)(msec * impl->rate / 1000);
}
static float samples_to_msec(struct impl *impl, uint32_t samples)
{
	return samples * 1000.0f / impl->rate;
}

static void on_flush_timeout(void *d, uint64_t expirations)
{
	struct impl *impl = d;
	impl->flush_timeout(d, expirations);
}

static void default_reset_ringbuffer(struct impl *impl)
{
	spa_memzero(impl->buffer, sizeof(impl->buffer));
}

struct rtp_stream *rtp_stream_new(struct pw_core *core,
		enum spa_direction direction, struct pw_properties *props,
		const struct rtp_stream_events *events, void *data)
{
	struct impl *impl;
	const char *str, *aes67_driver;
	char tmp[64];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	uint32_t n_params, min_samples, max_samples;
	float min_ptime, max_ptime;
	const struct spa_pod *params[1];
	enum pw_stream_flags flags;
	float latency_msec;
	int res;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		res = -errno;
		goto out;
	}
	impl->first = true;
	spa_hook_list_init(&impl->listener_list);
	impl->direction = direction;
	impl->stream_events = stream_events;
	impl->context = pw_core_get_context(core);
	impl->main_loop = pw_context_get_main_loop(impl->context);
	impl->data_loop = pw_context_acquire_loop(impl->context, &props->dict);
	impl->timer = pw_loop_add_timer(impl->data_loop, on_flush_timeout, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer");
		goto out;
	}

	impl->reset_ringbuffer = default_reset_ringbuffer;

	if ((str = pw_properties_get(props, "sess.media")) == NULL)
		str = "audio";

	if (spa_streq(str, "audio")) {
		impl->info.media_type = SPA_MEDIA_TYPE_audio;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		impl->payload = 127;
	}
	else if (spa_streq(str, "raop")) {
		impl->info.media_type = SPA_MEDIA_TYPE_audio;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		impl->payload = 0x60;
	}
	else if (spa_streq(str, "midi")) {
		impl->info.media_type = SPA_MEDIA_TYPE_application;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_control;
		impl->payload = 0x61;
	}
#ifdef HAVE_OPUS
	else if (spa_streq(str, "opus")) {
		impl->info.media_type = SPA_MEDIA_TYPE_audio;
		impl->info.media_subtype = SPA_MEDIA_SUBTYPE_opus;
		impl->payload = 127;
	}
#endif
	else {
		pw_log_error("unsupported media type:%s", str);
		res = -EINVAL;
		goto out;
	}

	switch (impl->info.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		parse_audio_info(props, &impl->info.info.raw);
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
		break;
	case SPA_MEDIA_SUBTYPE_opus:
		impl->stream_info.media_type = SPA_MEDIA_TYPE_audio;
		impl->stream_info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
		parse_audio_info(props, &impl->stream_info.info.raw);
		impl->stream_info.info.raw.format = SPA_AUDIO_FORMAT_F32;
		impl->info.info.opus.rate = impl->stream_info.info.raw.rate;
		impl->info.info.opus.channels = impl->stream_info.info.raw.channels;

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
		break;
	default:
		spa_assert_not_reached();
		break;
	}

	/* Limit the actual maximum buffer size to the maximum integer multiple
	 * amount of impl->stride that fits within BUFFER_SIZE. This is important
	 * to prevent corner cases where the read pointer wrapped around at the
	 * same time when the IO clock experiences a discontinuity.
	 *
	 * If the BUFFER_SIZE constant is not an integer multiple of impl->stride,
	 * pointer wrap-arounds will result in positions that exhibit a nonzero
	 * impl->stride division rest. Also, the write and read pointers are normally
	 * increased monotonically and contiguously. But, if a discontinuity is
	 * detected, these pointers may be resynchronized. Importantly, sometimes
	 * only one of them may be resynchronized, while the other retains its existing
	 * synchronization. (For example, the read and write side may use different
	 * discontinuity thresholds.)
	 *
	 * What then can happen is that the resynchronized pointer exhibits a _different_
	 * impl->stride division than the other pointer. Once the resynchronization takes
	 * place, that pointer is again monotonically increased from then on, so those
	 * division rests will stay different. This then means that the read and write
	 * operations will not be aligned properly. For example, a write operation might
	 * write to position 20 in the ring buffer, but the read operation might read
	 * from position 22, and doing so with a stride value of 6. The end result is
	 * invalid data.
	 *
	 * One way to visualize this is to think of the ring buffer as a grid. The grid
	 * cell size equals impl->stride. If BUFFER_SIZE is not an integer multiple of
	 * impl->stride, it means that the very last grid cell will have a size that is
	 * smaller than impl->stride. The unaligned read/write operations mean that the
	 * operations will not be done at the same grid cell boundaries, so for example
	 * the read operation might think that a cell starts at byte 2, while the write
	 * operation might think that the same cell starts at byte 4.
	 *
	 * By limiting the actual maximum buffer size to the maximum integer multiple
	 * amount of impl->stride that fits within BUFFER_SIZE, this is avoided, since
	 * then, all grid cells are guaranteed to have the size impl->stride, so the
	 * aforementioned division rest will always be zero.
	 */
	impl->actual_max_buffer_size = SPA_ROUND_DOWN(BUFFER_SIZE, impl->stride);
	pw_log_debug("possible / actual max buffer size: %" PRIu32 " / %" PRIu32,
			(uint32_t)BUFFER_SIZE, impl->actual_max_buffer_size);

	pw_properties_setf(props, "rtp.mime", "%s", impl->format_info->mime);

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");

	impl->marker_on_first = pw_properties_get_bool(props, "sess.marker-on-first", false);
	if (spa_streq(str, "raop"))
		impl->marker_on_first = 1;
	impl->ignore_ssrc = pw_properties_get_bool(props, "sess.ignore-ssrc", false);
	impl->direct_timestamp = pw_properties_get_bool(props, "sess.ts-direct", false);

	if (direction == PW_DIRECTION_INPUT) {
		impl->ssrc = pw_properties_get_uint32(props, "rtp.sender-ssrc", pw_rand32());
		impl->ts_offset = pw_properties_get_uint32(props, "rtp.sender-ts-offset", pw_rand32());
	} else {
		impl->have_ssrc = impl->fixed_ssrc = pw_properties_fetch_uint32(props, "rtp.receiver-ssrc", &impl->ssrc);
		if (pw_properties_fetch_uint32(props, "rtp.receiver-ts-offset", &impl->ts_offset) < 0)
			impl->direct_timestamp = false;
	}

	impl->payload = pw_properties_get_uint32(props, "rtp.payload", impl->payload);
	impl->mtu = pw_properties_get_uint32(props, "net.mtu", DEFAULT_MTU);
	impl->header_size = pw_properties_get_uint32(props, "net.header", IP4_HEADER_SIZE + UDP_HEADER_SIZE);
	impl->header_size += RTP_HEADER_SIZE;

	if (impl->mtu <= impl->header_size) {
		pw_log_error("invalid MTU %d, using %d", impl->mtu, DEFAULT_MTU);
		impl->mtu = DEFAULT_MTU;
	}
	impl->payload_size = impl->mtu - impl->header_size;

	impl->seq = pw_rand32();

	str = pw_properties_get(props, "sess.min-ptime");
	if (!spa_atof(str, &min_ptime))
		min_ptime = DEFAULT_MIN_PTIME;
	str = pw_properties_get(props, "sess.max-ptime");
	if (!spa_atof(str, &max_ptime))
		max_ptime = DEFAULT_MAX_PTIME;

	min_samples = msec_to_samples(impl, min_ptime);
	max_samples = msec_to_samples(impl, max_ptime);

	float ptime = 0.0f;
	if ((str = pw_properties_get(props, "rtp.ptime")) != NULL)
		if (!spa_atof(str, &ptime))
			ptime = 0.0f;

	uint32_t framecount = 0;
	if ((str = pw_properties_get(props, "rtp.framecount")) != NULL)
		if (!spa_atou32(str, &framecount, 0))
			framecount = 0;

	if (ptime > 0.0f || framecount > 0) {
		if (!framecount) {
			impl->psamples = msec_to_samples(impl, ptime);
			pw_properties_setf(props, "rtp.framecount", "%u", impl->psamples);
		} else if (ptime == 0.0f) {
			impl->psamples = framecount;
			pw_properties_set(props, "rtp.ptime",
					spa_dtoa(tmp, sizeof(tmp),
						samples_to_msec(impl, impl->psamples)));
		} else if (fabsf((samples_to_msec(impl, framecount)) - ptime) > 0.1f) {
			impl->psamples = msec_to_samples(impl, ptime);
			pw_log_warn("rtp.ptime doesn't match rtp.framecount. Choosing rtp.ptime");
		}
	} else {
		impl->psamples = impl->payload_size / impl->stride;
		impl->psamples = SPA_CLAMP(impl->psamples, min_samples, max_samples);
		if (direction == PW_DIRECTION_INPUT) {
			pw_properties_set(props, "rtp.ptime",
					spa_dtoa(tmp, sizeof(tmp),
						samples_to_msec(impl, impl->psamples)));

			pw_properties_setf(props, "rtp.framecount", "%u", impl->psamples);
		}
	}

	ptime = samples_to_msec(impl, impl->psamples);

	/* For senders, the default latency is ptime and for a receiver it is
	 * DEFAULT_SESS_LATENCY. Setting the sess.latency.msec for a sender to
	 * something smaller/bigger will influence the quantum and the amount
	 * of packets we send in one cycle */
	str = pw_properties_get(props, "sess.latency.msec");
	if (!spa_atof(str, &latency_msec)) {
		latency_msec = direction == PW_DIRECTION_INPUT ?
			ptime :
			DEFAULT_SESS_LATENCY;
	}
	impl->target_buffer = msec_to_samples(impl, latency_msec);
	impl->max_error = msec_to_samples(impl, ERROR_MSEC);

	if (impl->target_buffer < impl->psamples) {
		pw_log_warn("sess.latency.msec %f cannot be lower than rtp.ptime %f",
				latency_msec, ptime);
		impl->target_buffer = impl->psamples;
	}

	/* We're not expecting odd ptimes, so this modulo should be 0 */
	if (fmodf(impl->target_buffer, impl->psamples) != 0) {
		pw_log_warn("sess.latency.msec %f should be an integer multiple of rtp.ptime %f",
				latency_msec, ptime);
		impl->target_buffer = SPA_ROUND_DOWN(impl->target_buffer, impl->psamples);
	}

	aes67_driver = pw_properties_get(props, "aes67.driver-group");

	pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", impl->rate);
	if (direction == PW_DIRECTION_INPUT && !aes67_driver) {
		/* While sending, we accept latency-sized buffers, and break it
		 * up and send in ptime intervals using a timer */
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
				impl->target_buffer, impl->rate);
	} else {
		/* For receive, and with split sending, we break up the latency
		 * as half being in stream latency, and the rest in our own
		 * ringbuffer latency */
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
				impl->target_buffer / 2, impl->rate);
	}

	pw_properties_setf(props, "net.mtu", "%u", impl->mtu);
	pw_properties_setf(props, "rtp.media", "%s", impl->format_info->media_type);
	pw_properties_setf(props, "rtp.mime", "%s", impl->format_info->mime);
	pw_properties_setf(props, "rtp.payload", "%u", impl->payload);
	pw_properties_setf(props, "rtp.ssrc", "%u", impl->ssrc);
	pw_properties_setf(props, "rtp.rate", "%u", impl->rate);
	if (impl->info.info.raw.channels > 0)
		pw_properties_setf(props, "rtp.channels", "%u", impl->info.info.raw.channels);
	if ((str = pw_properties_get(props, "sess.ts-refclk")) != NULL) {
		pw_properties_setf(props, "rtp.ts-offset", "%u", impl->ts_offset);
		pw_properties_set(props, "rtp.ts-refclk", str);
	}

	spa_dll_init(&impl->dll);
	spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->rate);
	impl->corr = 1.0;

	impl->stream = pw_stream_new(core, "rtp-session", props);
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
		rtp_audio_init(impl, core, direction, aes67_driver);
		break;
	case SPA_MEDIA_SUBTYPE_control:
		params[n_params++] = spa_pod_builder_add_object(&b,
                                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                SPA_FORMAT_mediaType,           SPA_POD_Id(SPA_MEDIA_TYPE_application),
                                SPA_FORMAT_mediaSubtype,        SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		rtp_midi_init(impl, direction);
		break;
	case SPA_MEDIA_SUBTYPE_opus:
		params[n_params++] = spa_format_audio_build(&b,
				SPA_PARAM_EnumFormat, &impl->stream_info);
		flags |= PW_STREAM_FLAG_AUTOCONNECT;
		rtp_opus_init(impl, direction);
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

	return (struct rtp_stream*)impl;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

void rtp_stream_destroy(struct rtp_stream *s)
{
	struct impl *impl = (struct impl*)s;

	rtp_stream_emit_destroy(impl);

	if (impl->deinit)
		impl->deinit(impl, impl->direction);

	if (impl->ptp_sender)
		pw_filter_destroy(impl->ptp_sender);

	if (impl->stream)
		pw_stream_destroy(impl->stream);

	if (impl->timer)
		pw_loop_destroy_source(impl->data_loop, impl->timer);

	if (impl->data_loop)
		pw_context_release_loop(impl->context, impl->data_loop);

	spa_hook_list_clean(&impl->listener_list);
	free(impl);
}

int rtp_stream_update_properties(struct rtp_stream *s, const struct spa_dict *dict)
{
	struct impl *impl = (struct impl*)s;
	return pw_stream_update_properties(impl->stream, dict);
}

int rtp_stream_receive_packet(struct rtp_stream *s, uint8_t *buffer, size_t len)
{
	struct impl *impl = (struct impl*)s;
	return impl->receive_rtp(impl, buffer, len);
}

uint64_t rtp_stream_get_time(struct rtp_stream *s, uint32_t *rate)
{
	struct impl *impl = (struct impl*)s;
	struct spa_io_position *pos = impl->io_position;

	if (pos == NULL)
		return -EIO;

	*rate = impl->rate;
	return pos->clock.position * impl->rate *
		pos->clock.rate.num / pos->clock.rate.denom;
}

uint16_t rtp_stream_get_seq(struct rtp_stream *s)
{
	struct impl *impl = (struct impl*)s;
	return impl->seq;
}

size_t rtp_stream_get_mtu(struct rtp_stream *s)
{
	struct impl *impl = (struct impl*)s;
	return impl->mtu;
}

void rtp_stream_set_first(struct rtp_stream *s)
{
	struct impl *impl = (struct impl*)s;

	impl->first = true;
}

void rtp_stream_set_error(struct rtp_stream *s, int res, const char *error)
{
	struct impl *impl = (struct impl*)s;
	pw_stream_set_error(impl->stream, res, "%s: %s", error, spa_strerror(res));
}

enum pw_stream_state rtp_stream_get_state(struct rtp_stream *s, const char **error)
{
	struct impl *impl = (struct impl*)s;

	return pw_stream_get_state(impl->stream, error);
}
int rtp_stream_set_active(struct rtp_stream *s, bool active)
{
	struct impl *impl = (struct impl*)s;

	return pw_stream_set_active(impl->stream, active);
}

int rtp_stream_set_param(struct rtp_stream *s, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = (struct impl*)s;

	return pw_stream_set_param(impl->stream, id, param);
}

int rtp_stream_update_params(struct rtp_stream *s,
			const struct spa_pod **params,
			uint32_t n_params)
{
	struct impl *impl = (struct impl*)s;
	return pw_stream_update_params(impl->stream, params, n_params);
}
