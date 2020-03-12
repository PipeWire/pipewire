/* PCM - PipeWire plugin
 *
 * Copyright © 2017 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define __USE_GNU
#define _GNU_SOURCE

#include <limits.h>
#ifndef __FreeBSD__
#include <byteswap.h>
#endif
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#define NAME "alsa-plugin"

#define MIN_BUFFERS	3u
#define MAX_BUFFERS	64u

#define MAX_CHANNELS	32
#define MAX_RATE	(48000*8)

#define MIN_PERIOD	64

typedef struct {
	snd_pcm_ioplug_t io;

	char *node_name;
	uint32_t target;

	int fd;
	bool activated;		/* PipeWire is activated? */
	bool error;

	unsigned int num_ports;
	unsigned int hw_ptr;
	unsigned int sample_bits;
	snd_pcm_uframes_t min_avail;

	struct spa_system *system;
	struct pw_thread_loop *main_loop;

	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	uint32_t flags;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

        struct spa_audio_info_raw format;
} snd_pcm_pipewire_t;

static int snd_pcm_pipewire_stop(snd_pcm_ioplug_t *io);

static int pcm_poll_block_check(snd_pcm_ioplug_t *io)
{
	uint64_t val;
	snd_pcm_sframes_t avail;
	snd_pcm_pipewire_t *pw = io->private_data;

	if (io->state == SND_PCM_STATE_RUNNING ||
	    (io->state == SND_PCM_STATE_PREPARED && io->stream == SND_PCM_STREAM_CAPTURE)) {
		avail = snd_pcm_avail_update(io->pcm);
		if (avail >= 0 && avail < (snd_pcm_sframes_t)pw->min_avail) {
			spa_system_eventfd_read(pw->system, io->poll_fd, &val);
			return 1;
		}
	}

	return 0;
}

static inline int pcm_poll_unblock_check(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	spa_system_eventfd_write(pw->system, pw->fd, 1);
	return 1;
}

static void snd_pcm_pipewire_free(snd_pcm_pipewire_t *pw)
{
	if (pw) {
		if (pw->main_loop)
			pw_thread_loop_stop(pw->main_loop);
		if (pw->context)
			pw_context_destroy(pw->context);
		if (pw->fd >= 0)
			spa_system_close(pw->system, pw->fd);
		if (pw->main_loop)
			pw_thread_loop_destroy(pw->main_loop);
		free(pw);
	}
}

static int snd_pcm_pipewire_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_pipewire_free(pw);
	return 0;
}

static int snd_pcm_pipewire_poll_revents(snd_pcm_ioplug_t *io,
				     struct pollfd *pfds, unsigned int nfds,
				     unsigned short *revents)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	assert(pfds && nfds == 1 && revents);

	if (pw->error)
		return -EBADFD;

	*revents = pfds[0].revents & ~(POLLIN | POLLOUT);
	if (pfds[0].revents & POLLIN && !pcm_poll_block_check(io))
		*revents |= (io->stream == SND_PCM_STREAM_PLAYBACK) ? POLLOUT : POLLIN;

	return 0;
}

static snd_pcm_sframes_t snd_pcm_pipewire_pointer(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	if (pw->error)
		return -EBADFD;

	return pw->hw_ptr;
}

static int
snd_pcm_pipewire_process_playback(snd_pcm_pipewire_t *pw, struct pw_buffer *b)
{
	snd_pcm_ioplug_t *io = &pw->io;
	const snd_pcm_channel_area_t *areas;
	snd_pcm_channel_area_t *pwareas;
	snd_pcm_uframes_t xfer = 0;
	unsigned int channel, bps, bpf;
	snd_pcm_uframes_t nframes;
	uint32_t offset, index = 0, nbytes, avail, maxsize;
	int32_t filled;
	void *ptr;
	struct spa_data *d;

	bps = io->channels * pw->sample_bits;
	bpf = bps / 8;

	pwareas = alloca(io->channels * sizeof(snd_pcm_channel_area_t));

	d = b->buffer->datas;

	maxsize = d[0].maxsize;

	filled = 0;
	index = 0;
	avail = maxsize - filled;
	avail = SPA_MIN(avail, pw->min_avail * bpf);

	do {
	offset = index % maxsize;
	nbytes = SPA_MIN(avail, maxsize - offset);

	ptr = SPA_MEMBER(d[0].data, offset, void);

	nframes = nbytes / bpf;
	pw_log_trace(NAME" %p: %d %d %lu %d %d %p %d", pw, nbytes, avail, nframes, filled, offset, ptr, io->state);

	for (channel = 0; channel < io->channels; channel++) {
		pwareas[channel].addr = ptr;
		pwareas[channel].first = channel * pw->sample_bits;
		pwareas[channel].step = bps;
	}

	if (io->state != SND_PCM_STATE_RUNNING && io->state != SND_PCM_STATE_DRAINING) {
		pw_log_trace(NAME" %p: silence %lu frames %d", pw, nframes, io->state);
		for (channel = 0; channel < io->channels; channel++)
			snd_pcm_area_silence(&pwareas[channel], 0, nframes, io->format);
		goto done;
	}

	areas = snd_pcm_ioplug_mmap_areas(io);

	xfer = 0;
	while (xfer < nframes) {
		snd_pcm_uframes_t frames = nframes - xfer;
		snd_pcm_uframes_t offset = pw->hw_ptr;
		snd_pcm_uframes_t cont = io->buffer_size - offset;

		if (cont < frames)
			frames = cont;

		snd_pcm_areas_copy(pwareas, xfer,
				   areas, offset,
				   io->channels, frames, io->format);

		pw->hw_ptr += frames;
		pw->hw_ptr %= io->buffer_size;
		xfer += frames;
	}

	pcm_poll_unblock_check(io); /* unblock socket for polling if needed */

      done:
	index += nbytes;
	avail -= nbytes;
	} while (avail > 0);

	d[0].chunk->offset = 0;
	d[0].chunk->size = index;
	d[0].chunk->stride = 0;

	return 0;
}

static int
snd_pcm_pipewire_process_record(snd_pcm_pipewire_t *pw, struct pw_buffer *b)
{
	snd_pcm_ioplug_t *io = &pw->io;
	const snd_pcm_channel_area_t *areas;
	snd_pcm_channel_area_t *pwareas;
	snd_pcm_uframes_t xfer = 0;
	unsigned int channel, bps, bpf;
	snd_pcm_uframes_t nframes;
	uint32_t offset, index = 0, nbytes, avail, maxsize;
	struct spa_data *d;
	void *ptr;

	bps = io->channels * pw->sample_bits;
	bpf = bps / 8;

	pwareas = alloca(io->channels * sizeof(snd_pcm_channel_area_t));

	d = b->buffer->datas;

	maxsize = d[0].chunk->size;
	avail = maxsize;
	index = d[0].chunk->offset;

	do {
	avail = SPA_MIN(avail, pw->min_avail * bpf);
	offset = index % maxsize;
	nbytes = SPA_MIN(avail, maxsize - offset);
	ptr = SPA_MEMBER(d[0].data, offset, void);

	pw_log_trace(NAME" %p: %d %d %d %p", pw, nbytes, avail, offset, ptr);
	nframes = nbytes / bpf;

	for (channel = 0; channel < io->channels; channel++) {
		pwareas[channel].addr = ptr;
		pwareas[channel].first = channel * pw->sample_bits;
		pwareas[channel].step = bps;
	}

	areas = snd_pcm_ioplug_mmap_areas(io);

	xfer = 0;
	while (xfer < nframes) {
		snd_pcm_uframes_t frames = nframes - xfer;
		snd_pcm_uframes_t offset = pw->hw_ptr;
		snd_pcm_uframes_t cont = io->buffer_size - offset;

		if (cont < frames)
			frames = cont;

		snd_pcm_areas_copy(areas, offset,
				   pwareas, xfer,
				   io->channels, frames, io->format);

		pw->hw_ptr += frames;
		pw->hw_ptr %= io->buffer_size;
		xfer += frames;
	}

	pcm_poll_unblock_check(io); /* unblock socket for polling if needed */

	avail -= nbytes;
	index += nbytes;
	} while (avail > 0);

	return 0;
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	snd_pcm_pipewire_t *pw = data;
	snd_pcm_ioplug_t *io = &pw->io;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t stride = (io->channels * pw->sample_bits) / 8;
	uint32_t buffers;
	uint32_t size;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	io->period_size = pw->min_avail;
	buffers = SPA_CLAMP(io->buffer_size / io->period_size, MIN_BUFFERS, MAX_BUFFERS);
	size = io->period_size * stride;

	pw_log_info(NAME" %p: buffer_size:%lu period_size:%lu buffers:%u stride:%u size:%u min_avail:%lu",
			pw, io->buffer_size, io->period_size, buffers, stride, size, pw->min_avail);

	params[n_params++] = spa_pod_builder_add_object(&b,
	                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, MIN_BUFFERS, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(size, size, INT_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(stride),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

	pw_stream_update_params(pw->stream, params, n_params);
}

static void on_stream_process(void *data)
{
	snd_pcm_pipewire_t *pw = data;
	snd_pcm_ioplug_t *io = &pw->io;
	struct pw_buffer *b;

	b = pw_stream_dequeue_buffer(pw->stream);
	if (b == NULL)
		return;

	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		snd_pcm_pipewire_process_playback(pw, b);
	else
		snd_pcm_pipewire_process_record(pw, b);

	pw_stream_queue_buffer(pw->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
        .param_changed = on_stream_param_changed,
        .process = on_stream_process,
};

static int snd_pcm_pipewire_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_sw_params_t *swparams;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	int res;
	uint32_t min_period;

	pw_thread_loop_lock(pw->main_loop);

	snd_pcm_sw_params_alloca(&swparams);
	if ((res = snd_pcm_sw_params_current(io->pcm, swparams)) == 0)
		snd_pcm_sw_params_get_avail_min(swparams, &pw->min_avail);
	else
		pw->min_avail = io->period_size;

	min_period = (MIN_PERIOD * io->rate / 48000);
	pw->min_avail = SPA_MAX(pw->min_avail, min_period);

	pw_log_debug(NAME" %p: prepare %d %p %lu %ld", pw,
			pw->error, pw->stream, io->period_size, pw->min_avail);
	if (!pw->error && pw->stream != NULL)
		goto done;

	if (pw->stream != NULL) {
		pw_stream_destroy(pw->stream);
		pw->stream = NULL;
	}

	props = pw_properties_new(PW_KEY_CLIENT_API, "alsa", NULL);

	pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%lu/%u", pw->min_avail, io->rate);
	pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
	pw_properties_set(props, PW_KEY_MEDIA_CATEGORY,
			io->stream == SND_PCM_STREAM_PLAYBACK ?
			"Playback" : "Capture");
	pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Music");

	pw->stream = pw_stream_new(pw->core, pw->node_name, props);
	if (pw->stream == NULL)
		goto error;

	pw_stream_add_listener(pw->stream, &pw->stream_listener, &stream_events, pw);

	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &pw->format);
	pw->error = false;

	pw_stream_connect(pw->stream,
			  io->stream == SND_PCM_STREAM_PLAYBACK ?
				  PW_DIRECTION_OUTPUT :
				  PW_DIRECTION_INPUT,
			  pw->target,
			  pw->flags |
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS |
			  PW_STREAM_FLAG_RT_PROCESS,
			  params, 1);

done:
	pw->hw_ptr = 0;

	pw_thread_loop_unlock(pw->main_loop);

	return 0;

      error:
	pw_thread_loop_unlock(pw->main_loop);
	return -ENOMEM;
}

static int snd_pcm_pipewire_start(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_thread_loop_lock(pw->main_loop);
	if (!pw->activated && pw->stream != NULL) {
		pw_stream_set_active(pw->stream, true);
		pw->activated = true;
	}
	pw_thread_loop_unlock(pw->main_loop);
	return 0;
}

static int snd_pcm_pipewire_stop(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_thread_loop_lock(pw->main_loop);
	if (pw->activated && pw->stream != NULL) {
		pw_stream_set_active(pw->stream, false);
		pw->activated = false;
	}
	pw_thread_loop_unlock(pw->main_loop);
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
	case 7:
		info->position[5] = SPA_AUDIO_CHANNEL_SL;
		info->position[6] = SPA_AUDIO_CHANNEL_SR;
		/* Fall through */
	case 5:
		info->position[3] = SPA_AUDIO_CHANNEL_RL;
		info->position[4] = SPA_AUDIO_CHANNEL_RR;
		info->position[2] = SPA_AUDIO_CHANNEL_FC;
		info->position[0] = SPA_AUDIO_CHANNEL_FL;
		info->position[1] = SPA_AUDIO_CHANNEL_FR;
		return 1;
	case 8:
		info->position[6] = SPA_AUDIO_CHANNEL_SL;
		info->position[7] = SPA_AUDIO_CHANNEL_SR;
		/* Fall through */
	case 6:
		info->position[4] = SPA_AUDIO_CHANNEL_RL;
		info->position[5] = SPA_AUDIO_CHANNEL_RR;
		/* Fall through */
	case 4:
		info->position[3] = SPA_AUDIO_CHANNEL_LFE;
		/* Fall through */
	case 3:
		info->position[2] = SPA_AUDIO_CHANNEL_FC;
		/* Fall through */
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

	pw_log_debug(NAME" %p: hw_params %lu %lu", pw, io->buffer_size, io->period_size);

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
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(chmap_info); i++)
		if (chmap_info[i].channel == channel)
			return chmap_info[i].pos;
        return SND_CHMAP_UNKNOWN;
}

static int snd_pcm_pipewire_set_chmap(snd_pcm_ioplug_t * io,
				const snd_pcm_chmap_t * map)
{
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

	maps = calloc(9, sizeof(*maps));
	make_map(maps,  0, 1, SND_CHMAP_MONO);
	make_map(maps,  1, 2, SND_CHMAP_FL, SND_CHMAP_FR);
	make_map(maps,  2, 3, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC);
	make_map(maps,  3, 4, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE);
	make_map(maps,  4, 5, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_RL, SND_CHMAP_RR);
	make_map(maps,  5, 6, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE, SND_CHMAP_RL, SND_CHMAP_RR);
	make_map(maps,  6, 7, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC,
			SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_RL, SND_CHMAP_RR);
	make_map(maps,  7, 8, SND_CHMAP_FL, SND_CHMAP_FR, SND_CHMAP_FC, SND_CHMAP_LFE,
			SND_CHMAP_SL, SND_CHMAP_SR, SND_CHMAP_RL, SND_CHMAP_RR);

	return maps;
}

static snd_pcm_ioplug_callback_t pipewire_pcm_callback = {
	.close = snd_pcm_pipewire_close,
	.start = snd_pcm_pipewire_start,
	.stop = snd_pcm_pipewire_stop,
	.pointer = snd_pcm_pipewire_pointer,
	.prepare = snd_pcm_pipewire_prepare,
	.poll_revents = snd_pcm_pipewire_poll_revents,
	.hw_params = snd_pcm_pipewire_hw_params,
	.set_chmap = snd_pcm_pipewire_set_chmap,
	.get_chmap = snd_pcm_pipewire_get_chmap,
	.query_chmaps = snd_pcm_pipewire_query_chmaps,
};

static int pipewire_set_hw_constraint(snd_pcm_pipewire_t *pw, int rate,
		snd_pcm_format_t format, int channels, int period_bytes)
{
	unsigned int access_list[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED,
		SND_PCM_ACCESS_MMAP_NONINTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED,
		SND_PCM_ACCESS_RW_NONINTERLEAVED
	};
	unsigned int format_list[] = {
		SND_PCM_FORMAT_FLOAT_LE,
		SND_PCM_FORMAT_FLOAT_BE,
		SND_PCM_FORMAT_S32_LE,
		SND_PCM_FORMAT_S32_BE,
		SND_PCM_FORMAT_S16_LE,
		SND_PCM_FORMAT_S16_BE,
		SND_PCM_FORMAT_S24_LE,
		SND_PCM_FORMAT_S24_BE,
		SND_PCM_FORMAT_S24_3LE,
		SND_PCM_FORMAT_S24_3BE,
		SND_PCM_FORMAT_U8,
	};
	int min_rate;
	int max_rate;
	int min_channels;
	int max_channels;
	int min_period_bytes;
	int max_period_bytes;
	int err;

	if (rate > 0) {
		min_rate = max_rate = rate;
	} else {
		min_rate = 1;
		max_rate = MAX_RATE;
	}
	if (channels > 0) {
		min_channels = max_channels = channels;
	} else {
		min_channels = 1;
		max_channels = MAX_CHANNELS;
	}
	if (period_bytes > 0) {
		min_period_bytes = max_period_bytes = period_bytes;
	} else {
		min_period_bytes = 128;
		max_period_bytes = 2*1024*1024;
	}

	if ((err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_ACCESS,
						 SPA_N_ELEMENTS(access_list), access_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_CHANNELS,
						   min_channels, max_channels)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_RATE,
						   min_rate, max_rate)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
                                                   16*1024, 4*1024*1024)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io,
						   SND_PCM_IOPLUG_HW_PERIOD_BYTES,
						   min_period_bytes,
						   max_period_bytes)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_PERIODS,
						   3, 64)) < 0)
		return err;

	if (format != SND_PCM_FORMAT_UNKNOWN) {
		err = snd_pcm_ioplug_set_param_list(&pw->io,
				SND_PCM_IOPLUG_HW_FORMAT,
				1, (unsigned int *)&format);
		if (err < 0)
			return err;
	} else {
		err = snd_pcm_ioplug_set_param_list(&pw->io,
				SND_PCM_IOPLUG_HW_FORMAT,
				SPA_N_ELEMENTS(format_list),
				format_list);
		if (err < 0)
			return err;
	}

	return 0;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	snd_pcm_pipewire_t *pw = data;

	pw_log_error(NAME" %p: error id:%u seq:%d res:%d (%s): %s", pw,
			id, seq, res, spa_strerror(res), message);

	if (id == 0) {
		pw->error = true;
		if (pw->fd != -1)
			pcm_poll_unblock_check(&pw->io);
	}
	pw_thread_loop_signal(pw->main_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
        .error = on_core_error,
};

static int snd_pcm_pipewire_open(snd_pcm_t **pcmp, const char *name,
			     const char *node_name,
			     const char *playback_node,
			     const char *capture_node,
			     snd_pcm_stream_t stream,
			     int mode,
			     uint32_t flags,
			     int rate,
			     snd_pcm_format_t format,
			     int channels,
			     int period_bytes)
{
	snd_pcm_pipewire_t *pw;
	int err;
	const char *str;
	struct pw_properties *props;
	struct pw_loop *loop;

	assert(pcmp);
	pw = calloc(1, sizeof(*pw));
	if (!pw)
		return -ENOMEM;

	str = getenv("PIPEWIRE_NODE");

	pw_log_debug(NAME" %p: open %s %d %d %08x %d %s %d %d '%s'", pw, name,
			stream, mode, flags, rate,
			format != SND_PCM_FORMAT_UNKNOWN ? snd_pcm_format_name(format) : "none",
			channels, period_bytes, str);

	pw->fd = -1;
	pw->io.poll_fd = -1;
	pw->flags = flags;

	if (node_name == NULL)
		pw->node_name = spa_aprintf("ALSA %s",
			       stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture");
	else
		pw->node_name = strdup(node_name);

	if (pw->node_name == NULL)
		return -errno;

	pw->target = PW_ID_ANY;
	if (str != NULL)
		pw->target = atoi(str);
	else {
		if (stream == SND_PCM_STREAM_PLAYBACK)
			pw->target = playback_node ? (uint32_t)atoi(playback_node) : PW_ID_ANY;
		else
			pw->target = capture_node ? (uint32_t)atoi(capture_node) : PW_ID_ANY;
	}

	pw->main_loop = pw_thread_loop_new("alsa-pipewire", NULL);
	loop = pw_thread_loop_get_loop(pw->main_loop);
	pw->system = loop->system;
	pw->context = pw_context_new(loop, NULL, 0);

	props = pw_properties_new(NULL, NULL);
	str = pw_get_prgname();
	if (str)
		pw_properties_setf(props, PW_KEY_APP_NAME, "ALSA plug-in [%s]", str);
	else
		pw_properties_set(props, PW_KEY_APP_NAME, "ALSA plug-in");

	if ((err = pw_thread_loop_start(pw->main_loop)) < 0)
		goto error;

	pw_thread_loop_lock(pw->main_loop);
	pw->core = pw_context_connect(pw->context, props, 0);
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

	if ((err = snd_pcm_ioplug_create(&pw->io, name, stream, mode)) < 0)
		goto error;

	pw_log_debug(NAME" %p: open %s %d %d", pw, name, pw->io.stream, mode);

	if ((err = pipewire_set_hw_constraint(pw, rate, format, channels,
					period_bytes)) < 0)
		goto error;

	*pcmp = pw->io.pcm;

	return 0;

      error:
	snd_pcm_pipewire_free(pw);
	return err;
}


SPA_EXPORT
SND_PCM_PLUGIN_DEFINE_FUNC(pipewire)
{
	snd_config_iterator_t i, next;
	const char *node_name = NULL;
	const char *server_name = NULL;
	const char *playback_node = NULL;
	const char *capture_node = NULL;
	snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
	int rate = 0;
	int channels = 0;
	int period_bytes = 0;
	uint32_t flags = 0;
	int err;

        pw_init(NULL, NULL);

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "name") == 0) {
			snd_config_get_string(n, &node_name);
			continue;
		}
		if (strcmp(id, "server") == 0) {
			snd_config_get_string(n, &server_name);
			continue;
		}
		if (strcmp(id, "playback_node") == 0) {
			snd_config_get_string(n, &playback_node);
			continue;
		}
		if (strcmp(id, "capture_node") == 0) {
			snd_config_get_string(n, &capture_node);
			continue;
		}
		if (strcmp(id, "exclusive") == 0) {
			if (snd_config_get_bool(n))
				flags |= PW_STREAM_FLAG_EXCLUSIVE;
			continue;
		}
		if (strcmp(id, "rate") == 0) {
			long val;

			if (snd_config_get_integer(n, &val) == 0)
				rate = val;
			else
				SNDERR("%s: invalid type", id);
			continue;
		}
		if (strcmp(id, "format") == 0) {
			const char *str;

			if (snd_config_get_string(n, &str) == 0) {
				format = snd_pcm_format_value(str);
				if (format == SND_PCM_FORMAT_UNKNOWN)
					SNDERR("%s: invalid value %s", id, str);
			} else {
				SNDERR("%s: invalid type", id);
			}
			continue;
		}
		if (strcmp(id, "channels") == 0) {
			long val;

			if (snd_config_get_integer(n, &val) == 0)
				channels = val;
			else
				SNDERR("%s: invalid type", id);
			continue;
		}
		if (strcmp(id, "period_bytes") == 0) {
			long val;

			if (snd_config_get_integer(n, &val) == 0)
				period_bytes = val;
			else
				SNDERR("%s: invalid type", id);
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	err = snd_pcm_pipewire_open(pcmp, name, node_name, playback_node,
			capture_node, stream, mode, flags, rate, format,
			channels, period_bytes);

	return err;
}

SPA_EXPORT
SND_PCM_PLUGIN_SYMBOL(pipewire);
