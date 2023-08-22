/* PCM - PipeWire plugin */
/* SPDX-FileCopyrightText: Copyright © 2017 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#define __USE_GNU

#include <limits.h>
#if !defined(__FreeBSD__) && !defined(__MidnightBSD__)
#include <byteswap.h>
#endif
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/props.h>
#include <spa/utils/atomic.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>

PW_LOG_TOPIC_STATIC(alsa_log_topic, "alsa.pcm");
#define PW_LOG_TOPIC_DEFAULT alsa_log_topic

#define MIN_BUFFERS	2u
#define MAX_BUFFERS	64u

#define MAX_CHANNELS	64
#define MAX_RATE	(48000*8)

#define MIN_PERIOD	64

#define MIN_PERIOD_BYTES	(128)
#define MAX_PERIOD_BYTES	(2*1024*1024)

#define MIN_BUFFER_BYTES	(2*MIN_PERIOD_BYTES)
#define MAX_BUFFER_BYTES	(2*MAX_PERIOD_BYTES)

typedef struct {
	snd_pcm_ioplug_t io;

	snd_output_t *output;
	FILE *log_file;

	int fd;
	int error;
	unsigned int activated:1;	/* PipeWire is activated? */
	unsigned int drained:1;
	unsigned int draining:1;
	unsigned int xrun_detected:1;
	unsigned int hw_params_changed:1;
	unsigned int active:1;
	unsigned int negotiated:1;

	snd_pcm_uframes_t hw_ptr;
	snd_pcm_uframes_t boundary;
	snd_pcm_uframes_t min_avail;
	unsigned int sample_bits;
	uint32_t blocks;
	uint32_t stride;

	struct spa_system *system;
	struct pw_thread_loop *main_loop;

	struct pw_properties *props;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	int64_t delay;
	uint64_t transfered;
	uint64_t buffered;
	int64_t now;
	uintptr_t seq;

	struct spa_audio_info_raw format;
} snd_pcm_pipewire_t;

static int snd_pcm_pipewire_stop(snd_pcm_ioplug_t *io);

static int check_active(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_sframes_t avail;
	bool active;

	avail = snd_pcm_ioplug_avail(io, pw->hw_ptr, io->appl_ptr);

	if (io->state == SND_PCM_STATE_DRAINING) {
		active = pw->drained;
	}
	else if (avail >= 0 && avail < (snd_pcm_sframes_t)pw->min_avail) {
		active = false;
	}
	else if (avail >= (snd_pcm_sframes_t)pw->min_avail) {
		active = true;
	} else {
		active = false;
	}
	if (pw->active != active) {
		pw_log_trace("%p: avail:%lu min-avail:%lu state:%s hw:%lu appl:%lu active:%d->%d state:%s",
			pw, avail, pw->min_avail, snd_pcm_state_name(io->state),
			pw->hw_ptr, io->appl_ptr, pw->active, active,
			snd_pcm_state_name(io->state));
	}
	return active;
}


static int update_active(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	pw->active = check_active(io);
	uint64_t val;

	if (pw->active || pw->error < 0)
		spa_system_eventfd_write(pw->system, io->poll_fd, 1);
	else
		spa_system_eventfd_read(pw->system, io->poll_fd, &val);

	return pw->active;
}

static void snd_pcm_pipewire_free(snd_pcm_pipewire_t *pw)
{
	if (pw == NULL)
		return;

	pw_log_debug("%p: free", pw);
	if (pw->main_loop)
		pw_thread_loop_stop(pw->main_loop);
	if (pw->stream)
		pw_stream_destroy(pw->stream);
	if (pw->context)
		pw_context_destroy(pw->context);
	if (pw->fd >= 0)
		spa_system_close(pw->system, pw->fd);
	if (pw->main_loop)
		pw_thread_loop_destroy(pw->main_loop);
	pw_properties_free(pw->props);
	snd_output_close(pw->output);
	fclose(pw->log_file);
	free(pw);
}

static int snd_pcm_pipewire_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	pw_log_debug("%p: close", pw);
	snd_pcm_pipewire_free(pw);
	return 0;
}

static int snd_pcm_pipewire_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int space)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	update_active(io);
	pfds->fd = pw->fd;
	pfds->events = POLLIN | POLLERR | POLLNVAL;
	return 1;
}

static int snd_pcm_pipewire_poll_revents(snd_pcm_ioplug_t *io,
				     struct pollfd *pfds, unsigned int nfds,
				     unsigned short *revents)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	assert(pfds && nfds == 1 && revents);

	if (pw->error < 0)
		return pw->error;

	*revents = pfds[0].revents & ~(POLLIN | POLLOUT);
	if (pfds[0].revents & POLLIN && check_active(io)) {
		*revents |= (io->stream == SND_PCM_STREAM_PLAYBACK) ? POLLOUT : POLLIN;
		update_active(io);
	}

	return 0;
}

static snd_pcm_sframes_t snd_pcm_pipewire_pointer(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	if (pw->xrun_detected)
		return -EPIPE;
	if (pw->error < 0)
		return pw->error;
	if (io->buffer_size == 0)
		return 0;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	return pw->hw_ptr;
#else
	return pw->hw_ptr % io->buffer_size;
#endif
}

static int snd_pcm_pipewire_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	uintptr_t seq1, seq2;
	int64_t elapsed = 0, delay, now, avail;
	struct timespec ts;
	int64_t diff;

	do {
		seq1 = SPA_SEQ_READ(pw->seq);

		delay = pw->delay + pw->transfered;
		now = pw->now;
		if (io->stream == SND_PCM_STREAM_PLAYBACK)
			avail = snd_pcm_ioplug_hw_avail(io, pw->hw_ptr, io->appl_ptr);
		else
			avail = snd_pcm_ioplug_avail(io, pw->hw_ptr, io->appl_ptr);

		seq2 = SPA_SEQ_READ(pw->seq);
	} while (!SPA_SEQ_READ_SUCCESS(seq1, seq2));

	if (now != 0 && (io->state == SND_PCM_STATE_RUNNING ||
	    io->state == SND_PCM_STATE_DRAINING)) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		diff = SPA_TIMESPEC_TO_NSEC(&ts) - now;
		elapsed = (io->rate * diff) / SPA_NSEC_PER_SEC;

		if (io->stream == SND_PCM_STREAM_PLAYBACK)
			delay -= SPA_MIN(elapsed, delay);
		else
			delay += SPA_MIN(elapsed, (int64_t)io->buffer_size);
	}

	*delayp = delay + avail;

	pw_log_trace("avail:%"PRIi64" filled %"PRIi64" elapsed:%"PRIi64" delay:%ld hw:%lu appl:%lu",
			avail, delay, elapsed, *delayp, pw->hw_ptr, io->appl_ptr);

	return 0;
}

static snd_pcm_uframes_t
snd_pcm_pipewire_process(snd_pcm_pipewire_t *pw, struct pw_buffer *b,
		snd_pcm_uframes_t *hw_avail,snd_pcm_uframes_t want)
{
	snd_pcm_ioplug_t *io = &pw->io;
	snd_pcm_channel_area_t *pwareas;
	snd_pcm_uframes_t xfer = 0;
	snd_pcm_uframes_t nframes;
	unsigned int channel;
	struct spa_data *d;
	void *ptr;
	uint32_t bl, offset, size;

	d = b->buffer->datas;
	pwareas = alloca(io->channels * sizeof(snd_pcm_channel_area_t));

	for (bl = 0; bl < pw->blocks; bl++) {
		if (io->stream == SND_PCM_STREAM_PLAYBACK) {
			size = SPA_MIN(d[bl].maxsize, pw->min_avail * pw->stride);
		} else {
			offset = SPA_MIN(d[bl].chunk->offset, d[bl].maxsize);
			size = SPA_MIN(d[bl].chunk->size, d[bl].maxsize - offset);
		}
		want = SPA_MIN(want, size / pw->stride);
	}
	nframes = SPA_MIN(want, *hw_avail);

	if (pw->blocks == 1) {
		if (io->stream == SND_PCM_STREAM_PLAYBACK) {
			d[0].chunk->size = want * pw->stride;
			d[0].chunk->offset = offset = 0;
		} else {
			offset = SPA_MIN(d[0].chunk->offset, d[0].maxsize);
		}
		ptr = SPA_PTROFF(d[0].data, offset, void);
		for (channel = 0; channel < io->channels; channel++) {
			pwareas[channel].addr = ptr;
			pwareas[channel].first = channel * pw->sample_bits;
			pwareas[channel].step = io->channels * pw->sample_bits;
		}
	} else {
		for (channel = 0; channel < io->channels; channel++) {
			if (io->stream == SND_PCM_STREAM_PLAYBACK) {
				d[channel].chunk->size = want * pw->stride;
				d[channel].chunk->offset = offset = 0;
			} else {
				offset = SPA_MIN(d[channel].chunk->offset, d[channel].maxsize);
			}
			ptr = SPA_PTROFF(d[channel].data, offset, void);
			pwareas[channel].addr = ptr;
			pwareas[channel].first = 0;
			pwareas[channel].step = pw->sample_bits;
		}
	}

	if (io->state == SND_PCM_STATE_RUNNING ||
		io->state == SND_PCM_STATE_DRAINING) {
		snd_pcm_uframes_t hw_ptr = pw->hw_ptr;
		xfer = nframes;
		if (xfer > 0) {
			const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
			const snd_pcm_uframes_t offset = hw_ptr % io->buffer_size;

			if (io->stream == SND_PCM_STREAM_PLAYBACK)
				snd_pcm_areas_copy_wrap(pwareas, 0, nframes,
						areas, offset,
						io->buffer_size,
						io->channels, xfer,
						io->format);
			else
				snd_pcm_areas_copy_wrap(areas, offset,
						io->buffer_size,
						pwareas, 0, nframes,
						io->channels, xfer,
						io->format);

			hw_ptr += xfer;
			if (hw_ptr >= pw->boundary)
				hw_ptr -= pw->boundary;
			pw->hw_ptr = hw_ptr;
			*hw_avail -= xfer;
		}
	}
	/* check if requested frames were copied */
	if (xfer < want) {
		/* always fill the not yet written PipeWire buffer with silence */
		if (io->stream == SND_PCM_STREAM_PLAYBACK) {
			const snd_pcm_uframes_t frames = want - xfer;

			snd_pcm_areas_silence(pwareas, xfer, io->channels,
							  frames, io->format);
			xfer += frames;
		}
		if (io->state == SND_PCM_STATE_RUNNING ||
			io->state == SND_PCM_STATE_DRAINING) {
			/* report Xrun to user application */
			pw->xrun_detected = true;
		}
	}
	return xfer;
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	snd_pcm_pipewire_t *pw = data;
	snd_pcm_ioplug_t *io = &pw->io;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t buffers, size;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	io->period_size = pw->min_avail;

	buffers = SPA_CLAMP(io->buffer_size / io->period_size, MIN_BUFFERS, MAX_BUFFERS);
	size = io->period_size * pw->stride;

	pw_log_info("%p: buffer_size:%lu period_size:%lu buffers:%u size:%u min_avail:%lu",
			pw, io->buffer_size, io->period_size, buffers, size, pw->min_avail);

	params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, MIN_BUFFERS, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(pw->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(size, size, INT_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(pw->stride));

	pw_stream_update_params(pw->stream, params, n_params);

	pw->negotiated = true;
	pw_thread_loop_signal(pw->main_loop, false);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	snd_pcm_pipewire_t *pw = data;

	if (state == PW_STREAM_STATE_ERROR) {
		pw_log_warn("%s", error);
		pw->error = -EIO;
		update_active(&pw->io);
	}
}

static void on_stream_drained(void *data)
{
	snd_pcm_pipewire_t *pw = data;
	pw->drained = true;
	pw->draining = false;
	pw_log_debug("%p: drained", pw);
	pw_thread_loop_signal(pw->main_loop, false);
}

static void on_stream_process(void *data)
{
	snd_pcm_pipewire_t *pw = data;
	snd_pcm_ioplug_t *io = &pw->io;
	struct pw_buffer *b;
	snd_pcm_uframes_t hw_avail, before, want, xfer;
	struct pw_time pwt;
	int64_t delay;

	pw_stream_get_time_n(pw->stream, &pwt, sizeof(pwt));

	delay = pwt.delay;
	if (pwt.rate.num != 0)
		delay = delay * io->rate * pwt.rate.num / pwt.rate.denom;

	before = hw_avail = snd_pcm_ioplug_hw_avail(io, pw->hw_ptr, io->appl_ptr);

	if (pw->drained)
		goto done;

	b = pw_stream_dequeue_buffer(pw->stream);
	if (b == NULL)
		return;

	want = b->requested ? b->requested : hw_avail;

	SPA_SEQ_WRITE(pw->seq);

	if (pw->now != pwt.now) {
		pw->transfered = pw->buffered;
		pw->buffered = 0;
	}

	xfer = snd_pcm_pipewire_process(pw, b, &hw_avail, want);

	pw->delay = delay;
	/* the buffer is now queued in the stream and consumed */
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		pw->transfered += xfer;

	/* more then requested data transfered, use them in next iteration */
	pw->buffered = (want == 0 || pw->transfered < want) ?  0 : (pw->transfered % want);

	pw->now = pwt.now;
	SPA_SEQ_WRITE(pw->seq);

	pw_log_trace("%p: avail-before:%lu avail:%lu want:%lu xfer:%lu hw:%lu appl:%lu",
			pw, before, hw_avail, want, xfer, pw->hw_ptr, io->appl_ptr);

	pw_stream_queue_buffer(pw->stream, b);

	if (io->state == SND_PCM_STATE_DRAINING && !pw->draining && hw_avail == 0) {
		if (io->stream == SND_PCM_STREAM_CAPTURE) {
			on_stream_drained (pw); /* since pw_stream does not call drained() for capture */
		} else {
			pw_stream_flush(pw->stream, true);
			pw->draining = true;
			pw->drained = false;
		}
	}
done:
	update_active(io);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_stream_param_changed,
	.state_changed = on_stream_state_changed,
	.process = on_stream_process,
	.drained = on_stream_drained,
};

static int pipewire_start(snd_pcm_pipewire_t *pw)
{
	if (!pw->activated && pw->stream != NULL) {
		pw_stream_set_active(pw->stream, true);
		pw->activated = true;
	}
	return 0;
}

static int snd_pcm_pipewire_drain(snd_pcm_ioplug_t *io)
{
	int res;
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_thread_loop_lock(pw->main_loop);
	pw_log_debug("%p: drain", pw);
	pw->drained = false;
	pw->draining = false;
	pipewire_start(pw);
	while (!pw->drained && pw->error >= 0 && pw->activated) {
		pw_thread_loop_wait(pw->main_loop);
	}
	res = pw->error;
	pw_thread_loop_unlock(pw->main_loop);
	return res;
}

static int snd_pcm_pipewire_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_sw_params_t *swparams;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t min_period;

	pw_thread_loop_lock(pw->main_loop);

	snd_pcm_sw_params_alloca(&swparams);
	if (snd_pcm_sw_params_current(io->pcm, swparams) == 0) {
		snd_pcm_sw_params_get_avail_min(swparams, &pw->min_avail);
		snd_pcm_sw_params_get_boundary(swparams, &pw->boundary);
		snd_pcm_sw_params_dump(swparams, pw->output);
		fflush(pw->log_file);
	} else {
		pw->min_avail = io->period_size;
		pw->boundary = io->buffer_size;
	}

	min_period = (MIN_PERIOD * io->rate / 48000);
	pw->min_avail = SPA_MAX(pw->min_avail, min_period);

	pw_log_debug("%p: prepare error:%d stream:%p buffer-size:%lu "
			"period-size:%lu min-avail:%ld", pw, pw->error,
			pw->stream, io->buffer_size, io->period_size, pw->min_avail);

	if (pw->error >= 0 && pw->stream != NULL && !pw->hw_params_changed)
		goto done;
	pw->hw_params_changed = false;

	pw_properties_setf(pw->props, PW_KEY_NODE_LATENCY, "%lu/%u", pw->min_avail, io->rate);
	pw_properties_setf(pw->props, PW_KEY_NODE_RATE, "1/%u", io->rate);

	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &pw->format);

	if (pw->stream != NULL) {
		pw_stream_update_properties(pw->stream, &pw->props->dict);
		pw_stream_update_params(pw->stream, params, 1);
		goto done;
	}

	pw->stream = pw_stream_new(pw->core, NULL, pw_properties_copy(pw->props));
	if (pw->stream == NULL)
		goto error;

	pw_stream_add_listener(pw->stream, &pw->stream_listener, &stream_events, pw);

	pw->error = 0;

	pw->negotiated = false;
	pw_stream_connect(pw->stream,
				io->stream == SND_PCM_STREAM_PLAYBACK ?
				PW_DIRECTION_OUTPUT :
				PW_DIRECTION_INPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);

done:
	pw->hw_ptr = 0;
	pw->now = 0;
	pw->xrun_detected = false;
	pw->drained = false;
	pw->draining = false;

	while (!pw->negotiated && pw->error >= 0)
		pw_thread_loop_wait(pw->main_loop);
	if (pw->error < 0)
		goto error;

	pw_thread_loop_unlock(pw->main_loop);

	return 0;

error:
	pw_thread_loop_unlock(pw->main_loop);
	return pw->error < 0 ? pw->error : -ENOMEM;
}

static int snd_pcm_pipewire_start(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_thread_loop_lock(pw->main_loop);
	pw_log_debug("%p: start", pw);
	pipewire_start(pw);
	pw_thread_loop_unlock(pw->main_loop);
	return 0;
}

static int snd_pcm_pipewire_stop(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_log_debug("%p: stop", pw);
	update_active(io);

	pw_thread_loop_lock(pw->main_loop);
	if (pw->activated && pw->stream != NULL) {
		pw_stream_set_active(pw->stream, false);
		pw->activated = false;
	}
	pw_thread_loop_unlock(pw->main_loop);
	return 0;
}

static int snd_pcm_pipewire_pause(snd_pcm_ioplug_t * io, int enable)
{
	pw_log_debug("%p: pause", io);

	if (enable)
		snd_pcm_pipewire_stop(io);
	else
		snd_pcm_pipewire_start(io);

	return 0;
}

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(p, fmt)  p ? SPA_AUDIO_FORMAT_UNKNOWN : SPA_AUDIO_FORMAT_ ## fmt ## _OE
#define _FORMAT_BE(p, fmt)  p ? SPA_AUDIO_FORMAT_ ## fmt ## P : SPA_AUDIO_FORMAT_ ## fmt
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(p, fmt)  p ? SPA_AUDIO_FORMAT_ ## fmt ## P : SPA_AUDIO_FORMAT_ ## fmt
#define _FORMAT_BE(p, fmt)  p ? SPA_AUDIO_FORMAT_UNKNOWN : SPA_AUDIO_FORMAT_ ## fmt ## _OE
#endif

static int set_default_channels(struct spa_audio_info_raw *info)
{
	switch (info->channels) {
	case 8:
		info->position[6] = SPA_AUDIO_CHANNEL_SL;
		info->position[7] = SPA_AUDIO_CHANNEL_SR;
		SPA_FALLTHROUGH
	case 6:
		info->position[5] = SPA_AUDIO_CHANNEL_LFE;
		SPA_FALLTHROUGH
	case 5:
		info->position[4] = SPA_AUDIO_CHANNEL_FC;
		SPA_FALLTHROUGH
	case 4:
		info->position[2] = SPA_AUDIO_CHANNEL_RL;
		info->position[3] = SPA_AUDIO_CHANNEL_RR;
		SPA_FALLTHROUGH
	case 2:
		info->position[0] = SPA_AUDIO_CHANNEL_FL;
		info->position[1] = SPA_AUDIO_CHANNEL_FR;
		return 1;
	case 1:
		info->position[0] = SPA_AUDIO_CHANNEL_MONO;
		return 1;
	default:
		return 0;
	}
}

static int snd_pcm_pipewire_hw_params(snd_pcm_ioplug_t * io,
				snd_pcm_hw_params_t * params)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	bool planar;

	snd_pcm_hw_params_dump(params, pw->output);
	fflush(pw->log_file);

	pw_log_debug("%p: hw_params buffer_size:%lu period_size:%lu", pw, io->buffer_size, io->period_size);

	switch(io->access) {
	case SND_PCM_ACCESS_MMAP_INTERLEAVED:
	case SND_PCM_ACCESS_RW_INTERLEAVED:
		planar = false;
		break;
	case SND_PCM_ACCESS_MMAP_NONINTERLEAVED:
	case SND_PCM_ACCESS_RW_NONINTERLEAVED:
		planar = true;
		break;
	default:
		SNDERR("PipeWire: invalid access: %d\n", io->access);
		return -EINVAL;
	}

	switch(io->format) {
	case SND_PCM_FORMAT_U8:
		pw->format.format = planar ? SPA_AUDIO_FORMAT_U8P : SPA_AUDIO_FORMAT_U8;
		break;
	case SND_PCM_FORMAT_S16_LE:
		pw->format.format = _FORMAT_LE(planar, S16);
		break;
	case SND_PCM_FORMAT_S16_BE:
		pw->format.format = _FORMAT_BE(planar, S16);
		break;
	case SND_PCM_FORMAT_S24_LE:
		pw->format.format = _FORMAT_LE(planar, S24_32);
		break;
	case SND_PCM_FORMAT_S24_BE:
		pw->format.format = _FORMAT_BE(planar, S24_32);
		break;
	case SND_PCM_FORMAT_S32_LE:
		pw->format.format = _FORMAT_LE(planar, S32);
		break;
	case SND_PCM_FORMAT_S32_BE:
		pw->format.format = _FORMAT_BE(planar, S32);
		break;
	case SND_PCM_FORMAT_S24_3LE:
		pw->format.format = _FORMAT_LE(planar, S24);
		break;
	case SND_PCM_FORMAT_S24_3BE:
		pw->format.format = _FORMAT_BE(planar, S24);
		break;
	case SND_PCM_FORMAT_FLOAT_LE:
		pw->format.format = _FORMAT_LE(planar, F32);
		break;
	case SND_PCM_FORMAT_FLOAT_BE:
		pw->format.format = _FORMAT_BE(planar, F32);
		break;
	default:
		SNDERR("PipeWire: invalid format: %d\n", io->format);
		return -EINVAL;
	}
	pw->format.channels = io->channels;
	pw->format.rate = io->rate;

	set_default_channels(&pw->format);

	pw->sample_bits = snd_pcm_format_physical_width(io->format);
	if (planar) {
		pw->blocks = io->channels;
		pw->stride = pw->sample_bits / 8;
	} else {
		pw->blocks = 1;
		pw->stride = (io->channels * pw->sample_bits) / 8;
	}
	pw->hw_params_changed = true;
	pw_log_info("%p: format:%s channels:%d rate:%d stride:%d blocks:%d", pw,
			spa_debug_type_find_name(spa_type_audio_format, pw->format.format),
			io->channels, io->rate, pw->stride, pw->blocks);

	return 0;
}

static int snd_pcm_pipewire_sw_params(snd_pcm_ioplug_t * io,
				snd_pcm_sw_params_t * sw_params)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_thread_loop_lock(pw->main_loop);
	if (pw->stream) {
		snd_pcm_uframes_t min_avail;
		snd_pcm_sw_params_get_avail_min( sw_params, &min_avail);
		snd_pcm_sw_params_get_boundary(sw_params, &pw->boundary);
		if (min_avail != pw->min_avail) {
			char latency[64];
			struct spa_dict_item item[1];
			uint32_t min_period = (MIN_PERIOD * io->rate / 48000);

			pw->min_avail = SPA_MAX(min_avail, min_period);

			spa_scnprintf(latency, sizeof(latency), "%lu/%u", pw->min_avail, io->rate);
			item[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);

			pw_log_debug("%p: sw_params update props %p %ld", pw, pw->stream, pw->min_avail);
			pw_stream_update_properties(pw->stream, &SPA_DICT_INIT(item, 1));
		}
	} else {
		pw_log_debug("%p: sw_params pre-prepare noop", pw);
	}
	pw_thread_loop_unlock(pw->main_loop);

	return 0;
}

struct chmap_info {
	enum snd_pcm_chmap_position pos;
	enum spa_audio_channel channel;
};

static const struct chmap_info chmap_info[] = {
	[SND_CHMAP_UNKNOWN] = { SND_CHMAP_UNKNOWN, SPA_AUDIO_CHANNEL_UNKNOWN },
	[SND_CHMAP_NA] = { SND_CHMAP_NA, SPA_AUDIO_CHANNEL_NA },
	[SND_CHMAP_MONO] = { SND_CHMAP_MONO, SPA_AUDIO_CHANNEL_MONO },
	[SND_CHMAP_FL] = { SND_CHMAP_FL, SPA_AUDIO_CHANNEL_FL },
	[SND_CHMAP_FR] = { SND_CHMAP_FR, SPA_AUDIO_CHANNEL_FR },
	[SND_CHMAP_RL] = { SND_CHMAP_RL, SPA_AUDIO_CHANNEL_RL },
	[SND_CHMAP_RR] = { SND_CHMAP_RR, SPA_AUDIO_CHANNEL_RR },
	[SND_CHMAP_FC] = { SND_CHMAP_FC, SPA_AUDIO_CHANNEL_FC },
	[SND_CHMAP_LFE] = { SND_CHMAP_LFE, SPA_AUDIO_CHANNEL_LFE },
	[SND_CHMAP_SL] = { SND_CHMAP_SL, SPA_AUDIO_CHANNEL_SL },
	[SND_CHMAP_SR] = { SND_CHMAP_SR, SPA_AUDIO_CHANNEL_SR },
	[SND_CHMAP_RC] = { SND_CHMAP_RC, SPA_AUDIO_CHANNEL_RC },
	[SND_CHMAP_FLC] = { SND_CHMAP_FLC, SPA_AUDIO_CHANNEL_FLC },
	[SND_CHMAP_FRC] = { SND_CHMAP_FRC, SPA_AUDIO_CHANNEL_FRC },
	[SND_CHMAP_RLC] = { SND_CHMAP_RLC, SPA_AUDIO_CHANNEL_RLC },
	[SND_CHMAP_RRC] = { SND_CHMAP_RRC, SPA_AUDIO_CHANNEL_RRC },
	[SND_CHMAP_FLW] = { SND_CHMAP_FLW, SPA_AUDIO_CHANNEL_FLW },
	[SND_CHMAP_FRW] = { SND_CHMAP_FRW, SPA_AUDIO_CHANNEL_FRW },
	[SND_CHMAP_FLH] = { SND_CHMAP_FLH, SPA_AUDIO_CHANNEL_FLH },
	[SND_CHMAP_FCH] = { SND_CHMAP_FCH, SPA_AUDIO_CHANNEL_FCH },
	[SND_CHMAP_FRH] = { SND_CHMAP_FRH, SPA_AUDIO_CHANNEL_FRH },
	[SND_CHMAP_TC] = { SND_CHMAP_TC, SPA_AUDIO_CHANNEL_TC },
	[SND_CHMAP_TFL] = { SND_CHMAP_TFL, SPA_AUDIO_CHANNEL_TFL },
	[SND_CHMAP_TFR] = { SND_CHMAP_TFR, SPA_AUDIO_CHANNEL_TFR },
	[SND_CHMAP_TFC] = { SND_CHMAP_TFC, SPA_AUDIO_CHANNEL_TFC },
	[SND_CHMAP_TRL] = { SND_CHMAP_TRL, SPA_AUDIO_CHANNEL_TRL },
	[SND_CHMAP_TRR] = { SND_CHMAP_TRR, SPA_AUDIO_CHANNEL_TRR },
	[SND_CHMAP_TRC] = { SND_CHMAP_TRC, SPA_AUDIO_CHANNEL_TRC },
	[SND_CHMAP_TFLC] = { SND_CHMAP_TFLC, SPA_AUDIO_CHANNEL_TFLC },
	[SND_CHMAP_TFRC] = { SND_CHMAP_TFRC, SPA_AUDIO_CHANNEL_TFRC },
	[SND_CHMAP_TSL] = { SND_CHMAP_TSL, SPA_AUDIO_CHANNEL_TSL },
	[SND_CHMAP_TSR] = { SND_CHMAP_TSR, SPA_AUDIO_CHANNEL_TSR },
	[SND_CHMAP_LLFE] = { SND_CHMAP_LLFE, SPA_AUDIO_CHANNEL_LLFE },
	[SND_CHMAP_RLFE] = { SND_CHMAP_RLFE, SPA_AUDIO_CHANNEL_RLFE },
	[SND_CHMAP_BC] = { SND_CHMAP_BC, SPA_AUDIO_CHANNEL_BC },
	[SND_CHMAP_BLC] = { SND_CHMAP_BLC, SPA_AUDIO_CHANNEL_BLC },
	[SND_CHMAP_BRC] = { SND_CHMAP_BRC, SPA_AUDIO_CHANNEL_BRC },
};

static enum snd_pcm_chmap_position channel_to_chmap(enum spa_audio_channel channel)
{
	SPA_FOR_EACH_ELEMENT_VAR(chmap_info, info)
		if (info->channel == channel)
			return info->pos;
	return SND_CHMAP_UNKNOWN;
}

static enum spa_audio_channel chmap_to_channel(enum snd_pcm_chmap_position pos)
{
	if (pos >= SPA_N_ELEMENTS(chmap_info))
		return SPA_AUDIO_CHANNEL_UNKNOWN;
	return chmap_info[pos].channel;
}

static int snd_pcm_pipewire_set_chmap(snd_pcm_ioplug_t * io,
				const snd_pcm_chmap_t * map)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	unsigned int i;

	pw->format.channels = map->channels;
	for (i = 0; i < map->channels; i++) {
		pw->format.position[i] = chmap_to_channel(map->pos[i]);
		pw_log_debug("map %d: %s / %s", i,
				snd_pcm_chmap_name(map->pos[i]),
				spa_debug_type_find_short_name(spa_type_audio_channel,
					pw->format.position[i]));
	}
	return 1;
}

static snd_pcm_chmap_t * snd_pcm_pipewire_get_chmap(snd_pcm_ioplug_t * io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_chmap_t *map;
	uint32_t i;

	map = calloc(1, sizeof(snd_pcm_chmap_t) +
				 pw->format.channels * sizeof(unsigned int));
	map->channels = pw->format.channels;
	for (i = 0; i < pw->format.channels; i++)
		map->pos[i] = channel_to_chmap(pw->format.position[i]);

	return map;
}

static void make_map(snd_pcm_chmap_query_t **maps, int index, int channels, ...)
{
	va_list args;
	int i;

	maps[index] = malloc(sizeof(snd_pcm_chmap_query_t) + (channels * sizeof(unsigned int)));
	maps[index]->type = SND_CHMAP_TYPE_FIXED;
	maps[index]->map.channels = channels;
	va_start(args, channels);
	for (i = 0; i < channels; i++)
		maps[index]->map.pos[i] = va_arg(args, int);
	va_end(args);
}

static snd_pcm_chmap_query_t **snd_pcm_pipewire_query_chmaps(snd_pcm_ioplug_t *io)
{
	snd_pcm_chmap_query_t **maps;

	maps = calloc(7, sizeof(*maps));
	make_map(maps,  0, 1, SND_CHMAP_MONO);
	make_map(maps,  1, 2, SND_CHMAP_FL, SND_CHMAP_FR);
	make_map(maps,  2, 4, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR);
	make_map(maps,  3, 5, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR,
			SND_CHMAP_FC);
	make_map(maps,  4, 6, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR,
			SND_CHMAP_FC, SND_CHMAP_LFE);
	make_map(maps,  5, 8, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_RL, SND_CHMAP_RR,
			SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_SL, SND_CHMAP_SR);

	return maps;
}

static snd_pcm_ioplug_callback_t pipewire_pcm_callback = {
	.close = snd_pcm_pipewire_close,
	.start = snd_pcm_pipewire_start,
	.stop = snd_pcm_pipewire_stop,
	.pause = snd_pcm_pipewire_pause,
	.pointer = snd_pcm_pipewire_pointer,
	.delay = snd_pcm_pipewire_delay,
	.drain = snd_pcm_pipewire_drain,
	.prepare = snd_pcm_pipewire_prepare,
	.poll_descriptors = snd_pcm_pipewire_poll_descriptors,
	.poll_revents = snd_pcm_pipewire_poll_revents,
	.hw_params = snd_pcm_pipewire_hw_params,
	.sw_params = snd_pcm_pipewire_sw_params,
	.set_chmap = snd_pcm_pipewire_set_chmap,
	.get_chmap = snd_pcm_pipewire_get_chmap,
	.query_chmaps = snd_pcm_pipewire_query_chmaps,
};

#define MAX_VALS	64
struct param_info {
	const char *prop;
	int key;
#define TYPE_LIST	0
#define TYPE_MIN_MAX	1
	int type;
	unsigned int vals[MAX_VALS];
	unsigned int n_vals;
	int (*collect) (const char *str, int len, unsigned int *val);

};

static int collect_access(const char *str, int len, unsigned int *val)
{
	char key[64];

	if (spa_json_parse_stringn(str, len, key, sizeof(key)) <= 0)
		return -EINVAL;

	if (strcasecmp(key, "MMAP_INTERLEAVED") == 0)
		*val = SND_PCM_ACCESS_MMAP_INTERLEAVED;
	else if (strcasecmp(key, "MMAP_NONINTERLEAVED") == 0)
		*val = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
	else if (strcasecmp(key, "RW_INTERLEAVED") == 0)
		*val = SND_PCM_ACCESS_RW_INTERLEAVED;
	else if (strcasecmp(key, "RW_NONINTERLEAVED") == 0)
		*val = SND_PCM_ACCESS_RW_NONINTERLEAVED;
	else
		return -EINVAL;
	return 0;
}

static int collect_format(const char *str, int len, unsigned int *val)
{
	char key[64];
	snd_pcm_format_t fmt;

	if (spa_json_parse_stringn(str, len, key, sizeof(key)) < 0)
		return -EINVAL;

	fmt = snd_pcm_format_value(key);
	if (fmt != SND_PCM_FORMAT_UNKNOWN)
		*val = fmt;
	else
		return -EINVAL;
	return 0;
}

static int collect_int(const char *str, int len, unsigned int *val)
{
	int v;
	if (spa_json_parse_int(str, len, &v) > 0)
		*val = v;
	else
		return -EINVAL;
	return 0;
}

struct param_info infos[] = {
	{ "alsa.access", SND_PCM_IOPLUG_HW_ACCESS, TYPE_LIST,
		{ SND_PCM_ACCESS_MMAP_INTERLEAVED,
			SND_PCM_ACCESS_MMAP_NONINTERLEAVED,
			SND_PCM_ACCESS_RW_INTERLEAVED,
			SND_PCM_ACCESS_RW_NONINTERLEAVED }, 4, collect_access },
	{ "alsa.format", SND_PCM_IOPLUG_HW_FORMAT, TYPE_LIST,
		{
#if __BYTE_ORDER == __LITTLE_ENDIAN
			SND_PCM_FORMAT_FLOAT_LE,
			SND_PCM_FORMAT_S32_LE,
			SND_PCM_FORMAT_S24_LE,
			SND_PCM_FORMAT_S24_3LE,
			SND_PCM_FORMAT_S24_3BE,
			SND_PCM_FORMAT_S16_LE,
#elif __BYTE_ORDER == __BIG_ENDIAN
			SND_PCM_FORMAT_FLOAT_BE,
			SND_PCM_FORMAT_S32_BE,
			SND_PCM_FORMAT_S24_BE,
			SND_PCM_FORMAT_S24_3LE,
			SND_PCM_FORMAT_S24_3BE,
			SND_PCM_FORMAT_S16_BE,
#endif
			SND_PCM_FORMAT_U8 }, 7, collect_format },
	{ "alsa.rate", SND_PCM_IOPLUG_HW_RATE, TYPE_MIN_MAX,
		{ 1, MAX_RATE }, 2, collect_int },
	{ "alsa.channels", SND_PCM_IOPLUG_HW_CHANNELS, TYPE_MIN_MAX,
		{ 1, MAX_CHANNELS }, 2, collect_int },
	{ "alsa.buffer-bytes", SND_PCM_IOPLUG_HW_BUFFER_BYTES, TYPE_MIN_MAX,
		{ MIN_BUFFER_BYTES, MAX_BUFFER_BYTES }, 2, collect_int },
	{ "alsa.period-bytes", SND_PCM_IOPLUG_HW_PERIOD_BYTES, TYPE_MIN_MAX,
		{ MIN_PERIOD_BYTES, MAX_PERIOD_BYTES }, 2, collect_int },
	{ "alsa.periods", SND_PCM_IOPLUG_HW_PERIODS, TYPE_MIN_MAX,
		{ MIN_BUFFERS, 1024 }, 2, collect_int },
};

static struct param_info *param_info_by_key(int key)
{
	SPA_FOR_EACH_ELEMENT_VAR(infos, p) {
		if (p->key == key)
			return p;
	}
	return NULL;
}

static int parse_value(const char *str, struct param_info *info)
{
	struct spa_json it[2];
	unsigned int v;
	const char *val;
	int len;

	spa_json_init(&it[0], str, strlen(str));
	if ((len = spa_json_next(&it[0], &val)) <= 0)
		return -EINVAL;

	if (spa_json_is_array(val, len)) {
		info->type = TYPE_LIST;
		info->n_vals = 0;
		spa_json_enter(&it[0], &it[1]);
		while ((len = spa_json_next(&it[1], &val)) > 0 && info->n_vals < MAX_VALS) {
			if (info->collect(val, len, &v) < 0)
				continue;
			info->vals[info->n_vals++] = v;
		}
	}
	else if (spa_json_is_object(val, len)) {
		char key[64];
		info->type = TYPE_MIN_MAX;
		info->n_vals = 2;
		spa_json_enter(&it[0], &it[1]);
                while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
                        if ((len = spa_json_next(&it[1], &val)) <= 0)
                                break;
			if (info->collect(val, len, &v) < 0)
				continue;
			if (spa_streq(key, "min"))
				info->vals[0] = v;
			else if (spa_streq(key, "max"))
				info->vals[1] = v;
                }
	}
	else if (info->collect(val, len, &v) >= 0) {
		info->type = TYPE_LIST;
		info->vals[0] = v;
		info->n_vals = 1;
	}
	return 0;
}

static int set_constraint(snd_pcm_pipewire_t *pw, int key)
{
	struct param_info *p = param_info_by_key(key), info;
	const char *str;
	int err;

	if (p == NULL)
		return -EINVAL;

	info = *p;

	str = pw_properties_get(pw->props, p->prop);
	if (str != NULL)
		parse_value(str, &info);

	switch (info.type) {
	case TYPE_LIST:
		pw_log_info("%s: list %d", p->prop, info.n_vals);
		err = snd_pcm_ioplug_set_param_list(&pw->io, key, info.n_vals, info.vals);
		break;
	case TYPE_MIN_MAX:
		pw_log_info("%s: min:%u max:%u", p->prop, info.vals[0], info.vals[1]);
		err = snd_pcm_ioplug_set_param_minmax(&pw->io, key, info.vals[0], info.vals[1]);
		break;
	default:
		return -EIO;
	}
	if (err < 0)
		pw_log_warn("Can't set param %s: %s", info.prop, snd_strerror(err));

	return err;

}
static int pipewire_set_hw_constraint(snd_pcm_pipewire_t *pw)
{
	int err;
	if ((err = set_constraint(pw, SND_PCM_IOPLUG_HW_ACCESS)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_FORMAT)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_RATE)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_CHANNELS)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_PERIOD_BYTES)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_BUFFER_BYTES)) < 0 ||
	    (err = set_constraint(pw, SND_PCM_IOPLUG_HW_PERIODS)) < 0)
		return err;
	return 0;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	snd_pcm_pipewire_t *pw = data;

	pw_log_warn("%p: error id:%u seq:%d res:%d (%s): %s", pw,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE) {
		pw->error = res;
		if (pw->fd != -1)
			update_active(&pw->io);
	}
	pw_thread_loop_signal(pw->main_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};


static ssize_t log_write(void *cookie, const char *buf, size_t size)
{
	int len;

	while (size > 0) {
		len = strcspn(buf, "\n");
		if (len > 0)
			pw_log_debug("%.*s", (int)len, buf);
		buf += len + 1;
		size -= len + 1;
	}
	return size;
}

static cookie_io_functions_t io_funcs = {
	.write = log_write,
};

static int execute_match(void *data, const char *location, const char *action,
                const char *val, size_t len)
{
	snd_pcm_pipewire_t *pw = data;
	if (spa_streq(action, "update-props"))
		pw_properties_update_string(pw->props, val, len);
	return 1;
}

static int snd_pcm_pipewire_open(snd_pcm_t **pcmp,
		struct pw_properties *props, snd_pcm_stream_t stream, int mode)
{
	snd_pcm_pipewire_t *pw;
	int err;
	const char *str, *node_name = NULL;
	struct pw_loop *loop;

	assert(pcmp);
	pw = calloc(1, sizeof(*pw));
	if (!pw)
		return -ENOMEM;

	pw->props = props;
	pw->fd = -1;
	pw->io.poll_fd = -1;
	pw->log_file = fopencookie(pw, "w", io_funcs);
	if (pw->log_file == NULL) {
		pw_log_error("can't create log file: %m");
		err = -errno;
		goto error;
	}
	if ((err = snd_output_stdio_attach(&pw->output, pw->log_file, 0)) < 0) {
		pw_log_error("can't attach log file: %s", snd_strerror(err));
		goto error;
	}

	pw->main_loop = pw_thread_loop_new("alsa-pipewire", NULL);
	if (pw->main_loop == NULL) {
		err = -errno;
		goto error;
	}
	loop = pw_thread_loop_get_loop(pw->main_loop);
	pw->system = loop->system;
	if ((pw->context = pw_context_new(loop,
					pw_properties_new(
						PW_KEY_CONFIG_NAME, "client-rt.conf",
						PW_KEY_CLIENT_API, "alsa",
						NULL),
					0)) == NULL) {
		err = -errno;
		goto error;
	}

	pw_context_conf_update_props(pw->context, "alsa.properties", pw->props);

	pw_context_conf_section_match_rules(pw->context, "alsa.rules",
			&pw_context_get_properties(pw->context)->dict, execute_match, pw);

	if (pw_properties_get(pw->props, PW_KEY_APP_NAME) == NULL)
		pw_properties_setf(pw->props, PW_KEY_APP_NAME, "PipeWire ALSA [%s]",
				pw_get_prgname());
	if (pw_properties_get(pw->props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(pw->props, PW_KEY_NODE_NAME, "alsa_%s.%s",
			       stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
			       pw_get_prgname());
	if (pw_properties_get(pw->props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(pw->props, PW_KEY_NODE_DESCRIPTION, "ALSA %s [%s]",
			       stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture",
			       pw_get_prgname());
	if (pw_properties_get(pw->props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(pw->props, PW_KEY_MEDIA_NAME, "ALSA %s",
			       stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture");
	if (pw_properties_get(pw->props, PW_KEY_MEDIA_TYPE) == NULL)
		pw_properties_set(pw->props, PW_KEY_MEDIA_TYPE, "Audio");
	if (pw_properties_get(pw->props, PW_KEY_MEDIA_CATEGORY) == NULL)
		pw_properties_set(pw->props, PW_KEY_MEDIA_CATEGORY,
				stream == SND_PCM_STREAM_PLAYBACK ?
				"Playback" : "Capture");

	str = getenv("PIPEWIRE_ALSA");
	if (str != NULL)
		pw_properties_update_string(pw->props, str, strlen(str));

	str = getenv("PIPEWIRE_NODE");
	if (str != NULL && str[0])
		pw_properties_set(pw->props, PW_KEY_TARGET_OBJECT, str);

	node_name = pw_properties_get(pw->props, PW_KEY_NODE_NAME);
	if (pw_properties_get(pw->props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_set(pw->props, PW_KEY_MEDIA_NAME, node_name);

	if ((err = pw_thread_loop_start(pw->main_loop)) < 0)
		goto error;

	pw_thread_loop_lock(pw->main_loop);
	pw->core = pw_context_connect(pw->context, pw_properties_copy(pw->props), 0);
	if (pw->core == NULL) {
		err = -errno;
		pw_thread_loop_unlock(pw->main_loop);
		goto error;
	}
	pw_core_add_listener(pw->core, &pw->core_listener, &core_events, pw);
	pw_thread_loop_unlock(pw->main_loop);

	pw->fd = spa_system_eventfd_create(pw->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	pw->io.version = SND_PCM_IOPLUG_VERSION;
	pw->io.name = "ALSA <-> PipeWire PCM I/O Plugin";
	pw->io.callback = &pipewire_pcm_callback;
	pw->io.private_data = pw;
	pw->io.poll_fd = pw->fd;
	pw->io.poll_events = POLLIN;
	pw->io.mmap_rw = 1;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
	pw->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
#else
#warning hw_ptr updates of buffer_size will not be recognized by the ALSA library. Consider to update your ALSA library.
#endif
	pw->io.flags |= SND_PCM_IOPLUG_FLAG_MONOTONIC;

	if ((err = snd_pcm_ioplug_create(&pw->io, node_name, stream, mode)) < 0)
		goto error;

	if ((err = pipewire_set_hw_constraint(pw)) < 0)
		goto error;

	pw_log_debug("%p: opened name:%s stream:%s mode:%d", pw, node_name,
			snd_pcm_stream_name(pw->io.stream), mode);

	*pcmp = pw->io.pcm;

	return 0;

error:
	pw_log_debug("%p: failed to open %s :%s", pw, node_name, spa_strerror(err));
	snd_pcm_pipewire_free(pw);
	return err;
}


SPA_EXPORT
SND_PCM_PLUGIN_DEFINE_FUNC(pipewire)
{
	snd_config_iterator_t i, next;
	struct pw_properties *props;
	const char *str;
	long val;
	int err;

	pw_init(NULL, NULL);
	if (strstr(pw_get_library_version(), "0.2") != NULL)
		return -ENOTSUP;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return -errno;

	PW_LOG_TOPIC_INIT(alsa_log_topic);

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (spa_streq(id, "comment") || spa_streq(id, "type") || spa_streq(id, "hint"))
			continue;
		if (spa_streq(id, "name")) {
			if (snd_config_get_string(n, &str) == 0)
				pw_properties_set(props, PW_KEY_NODE_NAME, str);
			continue;
		}
		if (spa_streq(id, "server")) {
			if (snd_config_get_string(n, &str) == 0)
				pw_properties_set(props, PW_KEY_REMOTE_NAME, str);
			continue;
		}
		if (spa_streq(id, "playback_node")) {
			if (stream == SND_PCM_STREAM_PLAYBACK &&
			    snd_config_get_string(n, &str) == 0)
				if (str != NULL && !spa_streq(str, "-1"))
					pw_properties_set(props, PW_KEY_TARGET_OBJECT, str);
			continue;
		}
		if (spa_streq(id, "capture_node")) {
			if (stream == SND_PCM_STREAM_CAPTURE &&
			    snd_config_get_string(n, &str) == 0)
				if (str != NULL && !spa_streq(str, "-1"))
					pw_properties_set(props, PW_KEY_TARGET_OBJECT, str);
			continue;
		}
		if (spa_streq(id, "role")) {
			if (snd_config_get_string(n, &str) == 0)
				if (str != NULL && *str)
					pw_properties_set(props, PW_KEY_MEDIA_ROLE, str);
			continue;
		}
		if (spa_streq(id, "exclusive")) {
			if (snd_config_get_bool(n))
				pw_properties_set(props, PW_KEY_NODE_EXCLUSIVE, "true");
			continue;
		}
		if (spa_streq(id, "rate")) {
			if (snd_config_get_integer(n, &val) == 0) {
				if (val != 0)
					pw_properties_setf(props, "alsa.rate", "%ld", val);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		if (spa_streq(id, "format")) {
			if (snd_config_get_string(n, &str) == 0) {
				if (str != NULL && *str)
					pw_properties_set(props, "alsa.format", str);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		if (spa_streq(id, "channels")) {
			if (snd_config_get_integer(n, &val) == 0) {
				if (val != 0)
					pw_properties_setf(props, "alsa.channels", "%ld", val);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		if (spa_streq(id, "period_bytes")) {
			if (snd_config_get_integer(n, &val) == 0) {
				if (val != 0)
					pw_properties_setf(props, "alsa.period-bytes", "%ld", val);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		if (spa_streq(id, "buffer_bytes")) {
			long val;

			if (snd_config_get_integer(n, &val) == 0) {
				if (val != 0)
					pw_properties_setf(props, "alsa.buffer-bytes", "%ld", val);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		pw_properties_free(props);
		return -EINVAL;
	}

	err = snd_pcm_pipewire_open(pcmp, props, stream, mode);

	return err;
}

SPA_EXPORT
SND_PCM_PLUGIN_SYMBOL(pipewire);
