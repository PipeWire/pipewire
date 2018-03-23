/*
 *  PCM - PipeWire plugin
 *
 *  Copyright (c) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#define __USE_GNU
#define _GNU_SOURCE

#include <byteswap.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include <spa/support/type-map.h>
#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/node/io.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>

#define MAX_BUFFERS	8
#define MAX_CHANNELS	32
#define MAX_RATE	(48000*8)

struct type {
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
}

typedef enum _pipewire_format {
	SND_PCM_PIPEWIRE_FORMAT_RAW
} snd_pcm_pipewire_format_t;

typedef struct {
	snd_pcm_ioplug_t io;

	char *node_name;
	char *target;

	int fd;
	bool activated;		/* PipeWire is activated? */
	bool error;

	unsigned int num_ports;
	unsigned int hw_ptr;
	unsigned int sample_bits;
	snd_pcm_uframes_t min_avail;

	struct type type;

	struct pw_loop *loop;
	struct pw_thread_loop *main_loop;

	struct pw_core *core;
	struct pw_type *t;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

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
			read(io->poll_fd, &val, sizeof(val));
			return 1;
		}
	}

	return 0;
}

static int pcm_poll_unblock_check(snd_pcm_ioplug_t *io)
{
	uint64_t val = 1;
	snd_pcm_sframes_t avail;
	snd_pcm_pipewire_t *pw = io->private_data;

	avail = snd_pcm_avail_update(io->pcm);
	if (avail < 0 || avail >= (snd_pcm_sframes_t)pw->min_avail || pw->error) {
		write(pw->fd, &val, sizeof(val));
		return 1;
	}

	return 0;
}

static void snd_pcm_pipewire_free(snd_pcm_pipewire_t *pw)
{
	if (pw) {
		if (pw->main_loop)
			pw_thread_loop_stop(pw->main_loop);
		if (pw->core)
			pw_core_destroy(pw->core);
		if (pw->main_loop)
			pw_thread_loop_destroy(pw->main_loop);
		if (pw->loop)
			pw_loop_destroy(pw->loop);
		if (pw->fd >= 0)
			close(pw->fd);
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
	pw_log_trace("%d %d %d %d %p", nbytes, avail, filled, offset, ptr);

	nframes = nbytes / bpf;

	for (channel = 0; channel < io->channels; channel++) {
		pwareas[channel].addr = ptr;
		pwareas[channel].first = channel * pw->sample_bits;
		pwareas[channel].step = bps;
	}

	if (io->state != SND_PCM_STATE_RUNNING) {
		pw_log_trace("silence %lu frames", nframes);
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

	pw_log_trace("%d %d %d %p", nbytes, avail, offset, ptr);
	nframes = nbytes / bpf;

	for (channel = 0; channel < io->channels; channel++) {
		pwareas[channel].addr = ptr;
		pwareas[channel].first = channel * pw->sample_bits;
		pwareas[channel].step = bps;
	}

	if (io->state != SND_PCM_STATE_RUNNING) {
		if (io->stream == SND_PCM_STREAM_PLAYBACK) {
			pw_log_trace("slience %lu frames", nframes);
			for (channel = 0; channel < io->channels; channel++)
				snd_pcm_area_silence(&pwareas[channel], 0, nframes, io->format);
			goto done;
		}
	}

	areas = snd_pcm_ioplug_mmap_areas(io);

	xfer = 0;
	while (xfer < nframes) {
		snd_pcm_uframes_t frames = nframes - xfer;
		snd_pcm_uframes_t offset = pw->hw_ptr;
		snd_pcm_uframes_t cont = io->buffer_size - offset;

		if (cont < frames)
			frames = cont;

		if (io->stream == SND_PCM_STREAM_PLAYBACK)
			snd_pcm_areas_copy(pwareas, xfer,
					   areas, offset,
					   io->channels, frames, io->format);
		else
			snd_pcm_areas_copy(areas, offset,
					   pwareas, xfer,
					   io->channels, frames, io->format);

		pw->hw_ptr += frames;
		pw->hw_ptr %= io->buffer_size;
		xfer += frames;
	}

	pcm_poll_unblock_check(io); /* unblock socket for polling if needed */

      done:
	avail -= nbytes;
	index += nbytes;
	} while (avail > 0);

	return 0;
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
        .process = on_stream_process,
};

static int snd_pcm_pipewire_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_sw_params_t *swparams;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_type *t = pw->t;
	int res;

	pw_thread_loop_lock(pw->main_loop);

	if (pw->stream) {
		pw_stream_destroy(pw->stream);
		pw->stream = NULL;
	}

        pw->stream = pw_stream_new(pw->remote, pw->node_name, NULL);
	if (pw->stream == NULL)
		goto error;

	pw_stream_add_listener(pw->stream, &pw->stream_listener, &stream_events, pw);

	params[0] = spa_pod_builder_object(&b,
		t->param.idEnumFormat, t->spa_format,
		"I", pw->type.media_type.audio,
		"I", pw->type.media_subtype.raw,
		":", pw->type.format_audio.format,     "I", pw->format.format,
		":", pw->type.format_audio.channels,   "i", pw->format.channels,
		":", pw->type.format_audio.rate,       "i", pw->format.rate);

	spa_debug_pod(params[0], SPA_DEBUG_FLAG_FORMAT);

	pw_stream_connect(pw->stream,
			  PW_DIRECTION_OUTPUT,
			  pw->target,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS |
			  PW_STREAM_FLAG_RT_PROCESS,
			  params, 1);

	pw->hw_ptr = 0;

	snd_pcm_sw_params_alloca(&swparams);
	if ((res = snd_pcm_sw_params_current(io->pcm, swparams)) == 0)
		snd_pcm_sw_params_get_avail_min(swparams, &pw->min_avail);
	else
		pw->min_avail = io->period_size;

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
	if (!pw->activated) {
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
	if (pw->activated) {
		pw_stream_set_active(pw->stream, false);
		pw->activated = false;
	}
	pw_thread_loop_unlock(pw->main_loop);
	return 0;
}

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(s, fmt)  s . fmt ## _OE
#define _FORMAT_BE(s, fmt)  s . fmt
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(s, fmt)  s . fmt
#define _FORMAT_BE(s, fmt)  s . fmt ## _OE
#endif

static int snd_pcm_pipewire_hw_params(snd_pcm_ioplug_t * io,
				snd_pcm_hw_params_t * params)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	switch (io->format) {
	case SND_PCM_FORMAT_S8:
		pw->format.format = pw->type.audio_format.S8;
		break;
	case SND_PCM_FORMAT_U8:
		pw->format.format = pw->type.audio_format.U8;
		break;
        case SND_PCM_FORMAT_S16_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, S16);
		break;
	case SND_PCM_FORMAT_S16_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, S16);
		break;
	case SND_PCM_FORMAT_U16_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, U16);
		break;
	case SND_PCM_FORMAT_U16_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, U16);
		break;
	case SND_PCM_FORMAT_S24_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, S24_32);
		break;
	case SND_PCM_FORMAT_S24_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, S24_32);
		break;
	case SND_PCM_FORMAT_U24_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, U24_32);
		break;
	case SND_PCM_FORMAT_U24_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, U24_32);
		break;
	case SND_PCM_FORMAT_S32_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, S32);
		break;
	case SND_PCM_FORMAT_S32_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, S32);
		break;
	case SND_PCM_FORMAT_U32_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, U32);
		break;
	case SND_PCM_FORMAT_U32_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, U32);
		break;
	case SND_PCM_FORMAT_FLOAT_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, F32);
		break;
	case SND_PCM_FORMAT_FLOAT_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, F32);
		break;
	case SND_PCM_FORMAT_FLOAT64_LE:
		pw->format.format = _FORMAT_LE(pw->type.audio_format, F64);
		break;
	case SND_PCM_FORMAT_FLOAT64_BE:
		pw->format.format = _FORMAT_BE(pw->type.audio_format, F64);
		break;
	default:
		return -EINVAL;
	}
	pw->format.channels = io->channels;
	pw->format.rate = io->rate;

	pw->sample_bits = snd_pcm_format_physical_width(io->format);

	return 0;
}

static snd_pcm_ioplug_callback_t pipewire_pcm_callback = {
	.close = snd_pcm_pipewire_close,
	.start = snd_pcm_pipewire_start,
	.stop = snd_pcm_pipewire_stop,
	.pointer = snd_pcm_pipewire_pointer,
	.prepare = snd_pcm_pipewire_prepare,
	.poll_revents = snd_pcm_pipewire_poll_revents,
	.hw_params = snd_pcm_pipewire_hw_params,
};

static int pipewire_set_hw_constraint(snd_pcm_pipewire_t *pw)
{
	/* period and buffer bytes must be power of two */
	static const unsigned int bytes_list[] = {
		1U<<5, 1U<<6, 1U<<7, 1U<<8, 1U<<9, 1U<<10, 1U<<11, 1U<<12, 1U<<13,
		1U<<14, 1U<<15, 1U<<16, 1U<<17,1U<<18, 1U<<19, 1U<<20
	};
	unsigned int access_list[] = {
		SND_PCM_ACCESS_MMAP_INTERLEAVED,
		SND_PCM_ACCESS_MMAP_NONINTERLEAVED,
		SND_PCM_ACCESS_RW_INTERLEAVED,
		SND_PCM_ACCESS_RW_NONINTERLEAVED
	};
	unsigned int format_list[] = {
		SND_PCM_FORMAT_S16_LE,
#if 0
		SND_PCM_FORMAT_S8,
		SND_PCM_FORMAT_U8,
		SND_PCM_FORMAT_S16_LE,
		SND_PCM_FORMAT_S16_BE,
		SND_PCM_FORMAT_U16_LE,
		SND_PCM_FORMAT_U16_BE,
		SND_PCM_FORMAT_S24_LE,
		SND_PCM_FORMAT_S24_BE,
		SND_PCM_FORMAT_U24_LE,
		SND_PCM_FORMAT_U24_BE,
		SND_PCM_FORMAT_S32_LE,
		SND_PCM_FORMAT_S32_BE,
		SND_PCM_FORMAT_U32_LE,
		SND_PCM_FORMAT_U32_BE,
		SND_PCM_FORMAT_FLOAT_LE,
		SND_PCM_FORMAT_FLOAT_BE,
		SND_PCM_FORMAT_FLOAT64_LE,
		SND_PCM_FORMAT_FLOAT64_BE,
#endif
	};

	int err;

	if ((err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_ACCESS,
						 SPA_N_ELEMENTS(access_list), access_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_FORMAT,
						 SPA_N_ELEMENTS(format_list), format_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_CHANNELS,
						   1, MAX_CHANNELS)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_RATE,
						   1, MAX_RATE)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
						SPA_N_ELEMENTS(bytes_list), bytes_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
						SPA_N_ELEMENTS(bytes_list), bytes_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_PERIODS,
						   2, 64)) < 0)
		return err;

	return 0;
}

static void on_remote_state_changed(void *data, enum pw_remote_state old,
			enum pw_remote_state state, const char *error)
{
	snd_pcm_pipewire_t *pw = data;

        switch (state) {
        case PW_REMOTE_STATE_ERROR:
		pw->error = true;
		pcm_poll_unblock_check(&pw->io);
		/* fallthrough */
        case PW_REMOTE_STATE_CONNECTED:
                pw_thread_loop_signal(pw->main_loop, false);
                break;
	default:
                break;
        }
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
        .state_changed = on_remote_state_changed,
};

static int remote_connect_sync(snd_pcm_pipewire_t *pw)
{
	const char *error = NULL;
	enum pw_remote_state state;
	int res;

	pw_thread_loop_lock(pw->main_loop);

	if ((res = pw_remote_connect(pw->remote)) < 0) {
		error = spa_strerror(res);
		goto error;
	}

	while (true) {
		state = pw_remote_get_state(pw->remote, &error);
		if (state == PW_REMOTE_STATE_ERROR)
			goto error;

		if (state == PW_REMOTE_STATE_CONNECTED)
			break;

		pw_thread_loop_wait(pw->main_loop);
	}
     exit:
	pw_thread_loop_unlock(pw->main_loop);

	return res;

     error:
	SNDERR("PipeWire: Unable to connect: %s\n", error);
	goto exit;
}

static int snd_pcm_pipewire_open(snd_pcm_t **pcmp, const char *name,
			     const char *node_name,
			     const char *playback_node,
			     const char *capture_node,
			     snd_pcm_stream_t stream, int mode)
{
	snd_pcm_pipewire_t *pw;
	int err;
	static unsigned int num = 0;

	assert(pcmp);
	pw = calloc(1, sizeof(*pw));
	if (!pw)
		return -ENOMEM;

	pw_log_debug("open %s %d %d", name, stream, mode);

	pw->fd = -1;
	pw->io.poll_fd = -1;

	if (node_name == NULL)
		err = asprintf(&pw->node_name,
			       "alsa-pipewire.%s%s.%d.%d", name,
			       stream == SND_PCM_STREAM_PLAYBACK ? "P" : "C",
			       getpid(), num++);
	else
		pw->node_name = strdup(node_name);

	if (stream == SND_PCM_STREAM_PLAYBACK)
		pw->target = playback_node ? strdup(playback_node) : NULL;
	else
		pw->target = capture_node ? strdup(capture_node) : NULL;

        pw->loop = pw_loop_new(NULL);
        pw->main_loop = pw_thread_loop_new(pw->loop, "alsa-pipewire");
        pw->core = pw_core_new(pw->loop, NULL);
        pw->t = pw_core_get_type(pw->core);
        init_type(&pw->type, pw->t->map);
        spa_debug_set_type_map(pw->t->map);

	pw->remote = pw_remote_new(pw->core, NULL, 0);
	pw_remote_add_listener(pw->remote, &pw->remote_listener, &remote_events, pw);

	if ((err = pw_thread_loop_start(pw->main_loop)) < 0)
		goto error;

	if ((err = remote_connect_sync(pw)) < 0)
		goto error;

	pw->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	pw->io.version = SND_PCM_IOPLUG_VERSION;
	pw->io.name = "ALSA <-> PipeWire PCM I/O Plugin";
	pw->io.callback = &pipewire_pcm_callback;
	pw->io.private_data = pw;
	pw->io.poll_fd = pw->fd;
	pw->io.poll_events = POLLIN;
	pw->io.mmap_rw = 1;

	if ((err = snd_pcm_ioplug_create(&pw->io, name, stream, mode)) < 0)
		goto error;

	pw_log_debug("open %s %d %d", name, pw->io.stream, mode);

	if ((err = pipewire_set_hw_constraint(pw)) < 0)
		goto error;

	*pcmp = pw->io.pcm;

	return 0;

      error:
	snd_pcm_pipewire_free(pw);
	return err;
}


SND_PCM_PLUGIN_DEFINE_FUNC(pipewire)
{
	snd_config_iterator_t i, next;
	const char *node_name = NULL;
	const char *server_name = NULL;
	const char *playback_node = NULL;
	const char *capture_node = NULL;
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
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	err = snd_pcm_pipewire_open(pcmp, name, node_name, playback_node, capture_node, stream, mode);

	return err;
}

SND_PCM_PLUGIN_SYMBOL(pipewire);
