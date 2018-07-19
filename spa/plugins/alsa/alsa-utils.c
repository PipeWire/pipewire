#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>
#include <sys/timerfd.h>

#include <lib/debug.h>
#include <lib/pod.h>

#include "alsa-utils.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error(state->log, msg ": %s", snd_strerror(err)); return err; }

static int spa_alsa_open(struct state *state)
{
	int err;
	struct props *props = &state->props;

	if (state->opened)
		return 0;

	CHECK(snd_output_stdio_attach(&state->output, stderr, 0), "attach failed");

	spa_log_info(state->log, "ALSA device open '%s'", props->device);
	CHECK(snd_pcm_open(&state->hndl,
			   props->device,
			   state->stream,
			   SND_PCM_NONBLOCK |
			   SND_PCM_NO_AUTO_RESAMPLE |
			   SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT), "open failed");

	state->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	state->opened = true;
	state->sample_count = 0;

	return 0;
}

int spa_alsa_close(struct state *state)
{
	int err = 0;

	if (!state->opened)
		return 0;

	spa_log_info(state->log, "Device '%s' closing", state->props.device);
	CHECK(snd_pcm_close(state->hndl), "close failed");

	close(state->timerfd);
	state->opened = false;

	return err;
}

struct format_info {
	off_t format_offset;
	snd_pcm_format_t format;
};

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(fmt)  offsetof(struct type, audio_format. fmt ## _OE)
#define _FORMAT_BE(fmt)  offsetof(struct type, audio_format. fmt)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(fmt)  offsetof(struct type, audio_format. fmt)
#define _FORMAT_BE(fmt)  offsetof(struct type, audio_format. fmt ## _OE)
#endif

static const struct format_info format_info[] = {
	{offsetof(struct type, audio_format.UNKNOWN), SND_PCM_FORMAT_UNKNOWN},
	{offsetof(struct type, audio_format.S8), SND_PCM_FORMAT_S8},
	{offsetof(struct type, audio_format.U8), SND_PCM_FORMAT_U8},
	{_FORMAT_LE(S16), SND_PCM_FORMAT_S16_LE},
	{_FORMAT_BE(S16), SND_PCM_FORMAT_S16_BE},
	{_FORMAT_LE(U16), SND_PCM_FORMAT_U16_LE},
	{_FORMAT_BE(U16), SND_PCM_FORMAT_U16_BE},
	{_FORMAT_LE(S24_32), SND_PCM_FORMAT_S24_LE},
	{_FORMAT_BE(S24_32), SND_PCM_FORMAT_S24_BE},
	{_FORMAT_LE(U24_32), SND_PCM_FORMAT_U24_LE},
	{_FORMAT_BE(U24_32), SND_PCM_FORMAT_U24_BE},
	{_FORMAT_LE(S24), SND_PCM_FORMAT_S24_3LE},
	{_FORMAT_BE(S24), SND_PCM_FORMAT_S24_3BE},
	{_FORMAT_LE(U24), SND_PCM_FORMAT_U24_3LE},
	{_FORMAT_BE(U24), SND_PCM_FORMAT_U24_3BE},
	{_FORMAT_LE(S32), SND_PCM_FORMAT_S32_LE},
	{_FORMAT_BE(S32), SND_PCM_FORMAT_S32_BE},
	{_FORMAT_LE(U32), SND_PCM_FORMAT_U32_LE},
	{_FORMAT_BE(U32), SND_PCM_FORMAT_U32_BE},
	{_FORMAT_LE(F32), SND_PCM_FORMAT_FLOAT_LE},
	{_FORMAT_BE(F32), SND_PCM_FORMAT_FLOAT_BE},
	{_FORMAT_LE(F64), SND_PCM_FORMAT_FLOAT64_LE},
	{_FORMAT_BE(F64), SND_PCM_FORMAT_FLOAT64_BE},
};

static snd_pcm_format_t spa_alsa_format_to_alsa(struct type *map, uint32_t format)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		uint32_t f = *SPA_MEMBER(map, format_info[i].format_offset, uint32_t);
		if (f == format)
			return format_info[i].format;
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

int
spa_alsa_enum_format(struct state *state, uint32_t *index,
		     const struct spa_pod *filter,
		     struct spa_pod **result,
		     struct spa_pod_builder *builder)
{
	snd_pcm_t *hndl;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_mask_t *fmask;
	snd_pcm_access_mask_t *amask;
	int err, i, j, dir;
	unsigned int min, max;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_prop *prop;
	struct spa_pod *fmt;
	int res;
	bool opened;

	opened = state->opened;
	if ((err = spa_alsa_open(state)) < 0)
		return err;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (*index > 0) {
		res = 0;
		goto exit;
	}

	hndl = state->hndl;
	snd_pcm_hw_params_alloca(&params);
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration: no configurations available");

	spa_pod_builder_push_object(&b, state->type.param.idEnumFormat, state->type.format);
	spa_pod_builder_add(&b,
			"I", state->type.media_type.audio,
			"I", state->type.media_subtype.raw, 0);

	snd_pcm_format_mask_alloca(&fmask);
	snd_pcm_hw_params_get_format_mask(params, fmask);

	prop = spa_pod_builder_deref(&b,
		spa_pod_builder_push_prop(&b, state->type.format_audio.format,
			SPA_POD_PROP_RANGE_NONE));

	for (i = 1, j = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		const struct format_info *fi = &format_info[i];

		if (snd_pcm_format_mask_test(fmask, fi->format)) {
			uint32_t f = *SPA_MEMBER(&state->type, fi->format_offset, uint32_t);
			if (j++ == 0)
				spa_pod_builder_id(&b, f);
			spa_pod_builder_id(&b, f);
		}
	}
	if (j > 1)
		prop->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
	spa_pod_builder_pop(&b);

	snd_pcm_access_mask_alloca(&amask);
	snd_pcm_hw_params_get_access_mask(params, amask);

	prop = spa_pod_builder_deref(&b,
		spa_pod_builder_push_prop(&b, state->type.format_audio.layout,
			SPA_POD_PROP_RANGE_NONE));
	j = 0;
	if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
		if (j++ == 0)
			spa_pod_builder_int(&b, SPA_AUDIO_LAYOUT_INTERLEAVED);
		spa_pod_builder_int(&b, SPA_AUDIO_LAYOUT_INTERLEAVED);
	}
	if (snd_pcm_access_mask_test(amask, SND_PCM_ACCESS_MMAP_NONINTERLEAVED)) {
		if (j++ == 0)
			spa_pod_builder_int(&b, SPA_AUDIO_LAYOUT_NON_INTERLEAVED);
		spa_pod_builder_int(&b, SPA_AUDIO_LAYOUT_NON_INTERLEAVED);
	}
	if (j > 1)
		prop->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
	spa_pod_builder_pop(&b);

	CHECK(snd_pcm_hw_params_get_rate_min(params, &min, &dir), "get_rate_min");
	CHECK(snd_pcm_hw_params_get_rate_max(params, &max, &dir), "get_rate_max");

	prop = spa_pod_builder_deref(&b,
		spa_pod_builder_push_prop(&b, state->type.format_audio.rate, SPA_POD_PROP_RANGE_NONE));

	spa_pod_builder_int(&b, SPA_CLAMP(DEFAULT_RATE, min, max));
	if (min != max) {
		spa_pod_builder_int(&b, min);
		spa_pod_builder_int(&b, max);
		prop->body.flags |= SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET;
	}
	spa_pod_builder_pop(&b);

	CHECK(snd_pcm_hw_params_get_channels_min(params, &min), "get_channels_min");
	CHECK(snd_pcm_hw_params_get_channels_max(params, &max), "get_channels_max");

	prop = spa_pod_builder_deref(&b,
		spa_pod_builder_push_prop(&b, state->type.format_audio.channels, SPA_POD_PROP_RANGE_NONE));

	spa_pod_builder_int(&b, SPA_CLAMP(DEFAULT_CHANNELS, min, max));
	if (min != max) {
		spa_pod_builder_int(&b, min);
		spa_pod_builder_int(&b, max);
		prop->body.flags |= SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET;
	}
	spa_pod_builder_pop(&b);

	fmt = spa_pod_builder_pop(&b);

	(*index)++;

	if ((res = spa_pod_filter(builder, result, fmt, filter)) < 0)
		goto next;

	res = 1;

      exit:
	if (!opened)
		spa_alsa_close(state);
	return res;
}

int spa_alsa_set_format(struct state *state, struct spa_audio_info *fmt, uint32_t flags)
{
	unsigned int rrate, rchannels;
	snd_pcm_uframes_t period_size;
	int err, dir;
	snd_pcm_hw_params_t *params;
	snd_pcm_format_t format;
	struct spa_audio_info_raw *info = &fmt->info.raw;
	snd_pcm_t *hndl;
	unsigned int periods;

	if ((err = spa_alsa_open(state)) < 0)
		return err;

	hndl = state->hndl;

	snd_pcm_hw_params_alloca(&params);
	/* choose all parameters */
	CHECK(snd_pcm_hw_params_any(hndl, params), "Broken configuration for playback: no configurations available");
	/* set hardware resampling */
	CHECK(snd_pcm_hw_params_set_rate_resample(hndl, params, 0), "set_rate_resample");
	/* set the interleaved read/write format */
	CHECK(snd_pcm_hw_params_set_access(hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

	/* disable ALSA wakeups, we use a timer */
	if (snd_pcm_hw_params_can_disable_period_wakeup(params))
		CHECK(snd_pcm_hw_params_set_period_wakeup(hndl, params, 0), "set_period_wakeup");

	/* set the sample format */
	format = spa_alsa_format_to_alsa(&state->type, info->format);
	if (format == SND_PCM_FORMAT_UNKNOWN)
		return -EINVAL;

	spa_log_info(state->log, "Stream parameters are %iHz, %s, %i channels", info->rate, snd_pcm_format_name(format),
		     info->channels);
	CHECK(snd_pcm_hw_params_set_format(hndl, params, format), "set_format");

	/* set the count of channels */
	rchannels = info->channels;
	CHECK(snd_pcm_hw_params_set_channels_near(hndl, params, &rchannels), "set_channels");
	if (rchannels != info->channels) {
		spa_log_warn(state->log, "Channels doesn't match (requested %u, get %u", info->channels, rchannels);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->channels = rchannels;
		else
			return -EINVAL;
	}

	/* set the stream rate */
	rrate = info->rate;
	CHECK(snd_pcm_hw_params_set_rate_near(hndl, params, &rrate, 0), "set_rate_near");
	if (rrate != info->rate) {
		spa_log_warn(state->log, "Rate doesn't match (requested %iHz, get %iHz)", info->rate, rrate);
		if (flags & SPA_NODE_PARAM_FLAG_NEAREST)
			info->rate = rrate;
		else
			return -EINVAL;
	}

	state->format = format;
	state->channels = info->channels;
	state->rate = info->rate;
	state->frame_size = info->channels * (snd_pcm_format_physical_width(format) / 8);

	CHECK(snd_pcm_hw_params_get_buffer_size_max(params, &state->buffer_frames), "get_buffer_size_max");

	CHECK(snd_pcm_hw_params_set_buffer_size_near(hndl, params, &state->buffer_frames), "set_buffer_size_near");

	dir = 0;
	period_size = state->buffer_frames;
	CHECK(snd_pcm_hw_params_set_period_size_near(hndl, params, &period_size, &dir), "set_period_size_near");
	state->period_frames = period_size;
	periods = state->buffer_frames / state->period_frames;

	spa_log_info(state->log, "buffer frames %zd, period frames %zd, periods %u, frame_size %zd",
		     state->buffer_frames, state->period_frames, periods, state->frame_size);

	/* write the parameters to device */
	CHECK(snd_pcm_hw_params(hndl, params), "set_hw_params");

	return 0;
}

static int set_swparams(struct state *state)
{
	snd_pcm_t *hndl = state->hndl;
	int err = 0;
	snd_pcm_sw_params_t *params;
	snd_pcm_uframes_t boundary;

	snd_pcm_sw_params_alloca(&params);

	/* get the current params */
	CHECK(snd_pcm_sw_params_current(hndl, params), "sw_params_current");

	CHECK(snd_pcm_sw_params_set_tstamp_mode(hndl, params, SND_PCM_TSTAMP_ENABLE), "sw_params_set_tstamp_mode");

	/* start the transfer */
	CHECK(snd_pcm_sw_params_set_start_threshold(hndl, params, LONG_MAX), "set_start_threshold");
	CHECK(snd_pcm_sw_params_get_boundary(params, &boundary), "get_boundary");

	CHECK(snd_pcm_sw_params_set_stop_threshold(hndl, params, boundary), "set_stop_threshold");

	CHECK(snd_pcm_sw_params_set_period_event(hndl, params, 0), "set_period_event");

	/* write the parameters to the playback device */
	CHECK(snd_pcm_sw_params(hndl, params), "sw_params");

	return 0;
}

static inline void calc_timeout(size_t target, size_t current,
				size_t rate, snd_htimestamp_t *now,
				struct timespec *ts)
{
	ts->tv_sec = now->tv_sec;
	ts->tv_nsec = now->tv_nsec;
	if (target > current)
		ts->tv_nsec += ((target - current) * SPA_NSEC_PER_SEC) / rate;

	while (ts->tv_nsec >= SPA_NSEC_PER_SEC) {
		ts->tv_sec++;
		ts->tv_nsec -= SPA_NSEC_PER_SEC;
	}
}

static int set_timeout(struct state *state, size_t extra)
{
	struct itimerspec ts;

	calc_timeout(state->filled + extra, state->threshold, state->rate, &state->now, &ts.it_value);

	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = ((size_t)state->threshold * SPA_NSEC_PER_SEC) / state->rate;
	timerfd_settime(state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);

	return 0;
}

int spa_alsa_write(struct state *state, snd_pcm_uframes_t silence)
{
	snd_pcm_t *hndl = state->hndl;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_uframes_t written, frames, offset, to_write;
	int res;

	if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0) {
		spa_log_error(state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(res));
		return res;
	}
	spa_log_trace(state->log, "begin %ld %ld", offset, frames);

	silence = SPA_MIN(silence, frames);
	to_write = frames;
	written = 0;

	while (!spa_list_is_empty(&state->ready) && to_write > 0) {
		uint8_t *dst, *src;
		size_t n_bytes, n_frames;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, size, maxsize, l0, l1;

		b = spa_list_first(&state->ready, struct buffer, link);
		d = b->buf->datas;

		dst = SPA_MEMBER(my_areas[0].addr, offset * state->frame_size, uint8_t);
		src = d[0].data;

		size = d[0].chunk->size;
		maxsize = d[0].maxsize;

		index = d[0].chunk->offset + state->ready_offset;
		avail = size - state->ready_offset;
		avail /= state->frame_size;

		n_frames = SPA_MIN(avail, to_write);
		n_bytes = n_frames * state->frame_size;

		offs = index % maxsize;
		l0 = SPA_MIN(n_bytes, maxsize - offs);
		l1 = n_bytes - l0;

		memcpy(dst, src + offs, l0);
		if (l1 > 0)
			memcpy(dst + l0, src, l1);

		state->ready_offset += n_bytes;

		if (state->ready_offset >= size) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
			spa_log_trace(state->log, "alsa-util %p: reuse buffer %u", state, b->buf->id);
			state->callbacks->reuse_buffer(state->callbacks_data, 0, b->buf->id);
			state->ready_offset = 0;
		}
		written += n_frames;
		to_write -= n_frames;
		if (silence > n_frames)
			silence -= n_frames;
		else
			silence = 0;
	}
	if (written == 0)
		silence = SPA_MIN(to_write, state->threshold);

	if (silence > 0) {
		snd_pcm_areas_silence(my_areas, offset, state->channels, silence, state->format);
		written += silence;
	}

	spa_log_trace(state->log, "commit %ld %ld", offset, written);
	if ((res = snd_pcm_mmap_commit(hndl, offset, written)) < 0) {
		spa_log_error(state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(res));
		if (res != -EPIPE && res != -ESTRPIPE)
			return res;
	}
	state->sample_count += written;
	state->filled += written;

	if (!state->alsa_started && written > 0) {
		spa_log_trace(state->log, "snd_pcm_start");
		if ((res = snd_pcm_start(hndl)) < 0) {
			spa_log_error(state->log, "snd_pcm_start: %s", snd_strerror(res));
			return res;
		}
		state->alsa_started = true;
	}
	set_timeout(state, 0);

	return 0;
}

static snd_pcm_uframes_t
push_frames(struct state *state,
	    const snd_pcm_channel_area_t *my_areas,
	    snd_pcm_uframes_t offset,
	    snd_pcm_uframes_t frames)
{
	snd_pcm_uframes_t total_frames = 0;

	if (spa_list_is_empty(&state->free)) {
		spa_log_trace(state->log, "no more buffers");
	} else {
		uint8_t *src;
		size_t n_bytes;
		struct buffer *b;
		struct spa_data *d;
		uint32_t index, offs, avail, l0, l1;

		b = spa_list_first(&state->free, struct buffer, link);
		spa_list_remove(&b->link);

		if (b->h) {
			b->h->seq = state->sample_count;
			b->h->pts = SPA_TIMESPEC_TO_TIME(&state->now);
			b->h->dts_offset = 0;
		}

		d = b->buf->datas;

		src = SPA_MEMBER(my_areas[0].addr, offset * state->frame_size, uint8_t);

		avail = d[0].maxsize / state->frame_size;
		index = 0;
		total_frames = SPA_MIN(avail, frames);
		n_bytes = total_frames * state->frame_size;

		offs = index % d[0].maxsize;
		l0 = SPA_MIN(n_bytes, d[0].maxsize - offs);
		l1 = n_bytes - l0;

		memcpy(d[0].data + offs, src, l0);
		if (l1 > 0)
			memcpy(d[0].data, src + l0, l1);

		d[0].chunk->offset = index;
		d[0].chunk->size = n_bytes;
		d[0].chunk->stride = state->frame_size;

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
		spa_list_append(&state->ready, &b->link);

		state->callbacks->process(state->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	}
	return total_frames;
}

static int alsa_try_resume(struct state *state)
{
	int res;

	while ((res = snd_pcm_resume(state->hndl)) == -EAGAIN)
		usleep(250000);
	if (res < 0) {
		spa_log_error(state->log, "suspended, failed to resume %s", snd_strerror(res));
		res = snd_pcm_prepare(state->hndl);
		if (res < 0)
			spa_log_error(state->log, "suspended, failed to prepare %s", snd_strerror(res));
	}
	return res;
}

static void alsa_on_playback_timeout_event(struct spa_source *source)
{
	uint64_t exp;
	int res;
	struct state *state = source->data;
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_sframes_t avail;
	snd_pcm_status_t *status;

	if (state->started && read(state->timerfd, &exp, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(state->log, "error reading timerfd: %s", strerror(errno));

	snd_pcm_status_alloca(&status);

	if ((res = snd_pcm_status(hndl, status)) < 0) {
		spa_log_error(state->log, "snd_pcm_status error: %s", snd_strerror(res));
		return;
	}

	avail = snd_pcm_status_get_avail(status);
	snd_pcm_status_get_htstamp(status, &state->now);

	if (avail > state->buffer_frames)
		avail = state->buffer_frames;

	state->filled = state->buffer_frames - avail;

	if (state->clock) {
		state->clock->nsec = SPA_TIMESPEC_TO_TIME(&state->now);
		state->clock->rate = SPA_FRACTION(state->rate, 1);
		state->clock->position = state->sample_count;
		state->clock->delay = state->filled;
	}

	spa_log_trace(state->log, "timeout %ld %d %ld %ld %ld", state->filled, state->threshold,
		      state->sample_count, state->now.tv_sec, state->now.tv_nsec);

	if (state->filled > state->threshold) {
		if (snd_pcm_state(hndl) == SND_PCM_STATE_SUSPENDED) {
			spa_log_error(state->log, "suspended: try resume");
			if ((res = alsa_try_resume(state)) < 0)
				return;
		}
		set_timeout(state, 0);
	} else {
		if (spa_list_is_empty(&state->ready)) {
			struct spa_io_buffers *io = state->io;

			if (state->filled == 0) {
				if (state->alsa_started)
					spa_log_warn(state->log,
							"alsa-util %p: underrun", state);
				spa_alsa_write(state, state->threshold);
			}
			spa_log_trace(state->log, "alsa-util %p: %d %lu", state, io->status,
					state->filled);

			io->status = SPA_STATUS_NEED_BUFFER;
			if (state->range) {
				state->range->offset = state->sample_count * state->frame_size;
				state->range->min_size = state->threshold * state->frame_size;
				state->range->max_size = avail * state->frame_size;
			}
			state->callbacks->process(state->callbacks_data, SPA_STATUS_NEED_BUFFER);
		}
		else {
			spa_alsa_write(state, 0);
		}
	}
}


static void alsa_on_capture_timeout_event(struct spa_source *source)
{
	uint64_t exp;
	int res;
	struct state *state = source->data;
	snd_pcm_t *hndl = state->hndl;
	snd_pcm_sframes_t avail;
	snd_pcm_uframes_t total_read = 0;
	struct itimerspec ts;
	const snd_pcm_channel_area_t *my_areas;
	snd_pcm_status_t *status;
	struct timespec now;

	if (state->started && read(state->timerfd, &exp, sizeof(uint64_t)) != sizeof(uint64_t))
		spa_log_warn(state->log, "error reading timerfd: %s", strerror(errno));

	snd_pcm_status_alloca(&status);

	if ((res = snd_pcm_status(hndl, status)) < 0) {
		spa_log_error(state->log, "snd_pcm_status error: %s", snd_strerror(res));
		return;
	}

	avail = snd_pcm_status_get_avail(status);
	snd_pcm_status_get_htstamp(status, &state->now);
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (state->clock) {
		state->clock->nsec = SPA_TIMESPEC_TO_TIME(&state->now);
		state->clock->rate = SPA_FRACTION(state->rate, 1);
		state->clock->position = state->sample_count;
		state->clock->delay = avail;
	}

	spa_log_trace(state->log, "timeout %ld %d %ld %ld %ld %ld %ld", avail, state->threshold,
		      state->sample_count, state->now.tv_sec, state->now.tv_nsec,
		      now.tv_sec, now.tv_nsec);

	state->now = now;

	if (avail < state->threshold) {
		if (snd_pcm_state(hndl) == SND_PCM_STATE_SUSPENDED) {
			spa_log_error(state->log, "suspended: try resume");
			if ((res = alsa_try_resume(state)) < 0)
				return;
		}
	} else {
		snd_pcm_uframes_t to_read = SPA_MIN(avail, state->threshold);

		while (total_read < to_read) {
			snd_pcm_uframes_t read, frames, offset;

			frames = to_read - total_read;
			if ((res = snd_pcm_mmap_begin(hndl, &my_areas, &offset, &frames)) < 0) {
				spa_log_error(state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(res));
				return;
			}

			read = push_frames(state, my_areas, offset, frames);
			if (read < frames)
				to_read = 0;

			if ((res = snd_pcm_mmap_commit(hndl, offset, read)) < 0) {
				spa_log_error(state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(res));
				if (res != -EPIPE && res != -ESTRPIPE)
					return;
			}
			total_read += read;
		}
		state->sample_count += total_read;
	}
	calc_timeout(state->threshold, avail - total_read, state->rate, &state->now, &ts.it_value);

	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);
}

int spa_alsa_start(struct state *state, bool xrun_recover)
{
	int err;
	struct itimerspec ts;

	if (state->started)
		return 0;

	spa_log_debug(state->log, "alsa %p: start %d", state, state->threshold);

	CHECK(set_swparams(state), "swparams");
	if (!xrun_recover)
		snd_pcm_dump(state->hndl, state->output);

	if ((err = snd_pcm_prepare(state->hndl)) < 0) {
		spa_log_error(state->log, "snd_pcm_prepare error: %s", snd_strerror(err));
		return err;
	}

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		state->source.func = alsa_on_playback_timeout_event;
	} else {
		state->source.func = alsa_on_capture_timeout_event;
	}
	state->source.data = state;
	state->source.fd = state->timerfd;
	state->source.mask = SPA_IO_IN;
	state->source.rmask = 0;
	spa_loop_add_source(state->data_loop, &state->source);

	if (state->stream == SND_PCM_STREAM_PLAYBACK) {
		state->alsa_started = false;
	} else {
		if ((err = snd_pcm_start(state->hndl)) < 0) {
			spa_log_error(state->log, "snd_pcm_start: %s", snd_strerror(err));
			return err;
		}
		state->alsa_started = true;
	}

	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 1;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(state->timerfd, 0, &ts, NULL);

	state->io->status = SPA_STATUS_OK;
	state->io->buffer_id = SPA_ID_INVALID;

	state->started = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct state *state = user_data;
	struct itimerspec ts;

	spa_loop_remove_source(state->data_loop, &state->source);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	timerfd_settime(state->timerfd, 0, &ts, NULL);

	return 0;
}

int spa_alsa_pause(struct state *state, bool xrun_recover)
{
	int err;

	if (!state->started)
		return 0;

	spa_log_debug(state->log, "alsa %p: pause", state);

	spa_loop_invoke(state->data_loop, do_remove_source, 0, NULL, 0, true, state);

	if ((err = snd_pcm_drop(state->hndl)) < 0)
		spa_log_error(state->log, "snd_pcm_drop %s", snd_strerror(err));

	state->started = false;

	return 0;
}
