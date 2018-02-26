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

struct type {
	uint32_t format;
	uint32_t props;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
}

struct buffer {
	struct spa_buffer *buffer;
	struct spa_list link;
        struct spa_meta_header *h;
	void *ptr;
	bool mapped;
	bool used;
};

typedef enum _pipewire_format {
	SND_PCM_PIPEWIRE_FORMAT_RAW
} snd_pcm_pipewire_format_t;

typedef struct {
	snd_pcm_ioplug_t io;

	int fd;
	int activated;		/* PipeWire is activated? */
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

	struct pw_node *node;

        struct spa_node impl_node;
        const struct spa_node_callbacks *callbacks;
        void *callbacks_data;
        struct spa_io_buffers *port_io;
        struct spa_port_info port_info;

        struct spa_audio_info_raw format;

        struct buffer buffers[32];
        uint32_t n_buffers;
        struct spa_list empty;

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
	return pw->hw_ptr;
}

static int
snd_pcm_pipewire_process_playback(snd_pcm_pipewire_t *pw, struct buffer *b)
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

	ptr = SPA_MEMBER(b->ptr, offset, void);
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
snd_pcm_pipewire_process_record(snd_pcm_pipewire_t *pw, struct buffer *b)
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
	ptr = SPA_MEMBER(b->ptr, offset, void);

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

static int snd_pcm_pipewire_prepare(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;
	snd_pcm_sw_params_t *swparams;
	int err;

	pw->hw_ptr = 0;

	pw->min_avail = io->period_size;
	snd_pcm_sw_params_alloca(&swparams);
	err = snd_pcm_sw_params_current(io->pcm, swparams);
	if (err == 0) {
		snd_pcm_sw_params_get_avail_min(swparams, &pw->min_avail);
	}

	/* deactivate PipeWire connections if this is XRUN recovery */
	snd_pcm_pipewire_stop(io);

	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		pcm_poll_unblock_check(io); /* playback pcm initially accepts writes */
	else
		pcm_poll_block_check(io); /* block capture pcm if that's XRUN recovery */

	return 0;
}

static int snd_pcm_pipewire_start(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	pw_node_set_active(pw->node, true);
	pw->activated = 1;

	return 0;
}

static int snd_pcm_pipewire_stop(snd_pcm_ioplug_t *io)
{
	snd_pcm_pipewire_t *pw = io->private_data;

	if (pw->activated) {
		pw_node_set_active(pw->node, false);
		pw->activated = 0;
	}
	return 0;
}

static snd_pcm_ioplug_callback_t pipewire_pcm_callback = {
	.close = snd_pcm_pipewire_close,
	.start = snd_pcm_pipewire_start,
	.stop = snd_pcm_pipewire_stop,
	.pointer = snd_pcm_pipewire_pointer,
	.prepare = snd_pcm_pipewire_prepare,
	.poll_revents = snd_pcm_pipewire_poll_revents,
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
	unsigned int format = SND_PCM_FORMAT_S16_LE;
	unsigned int rate = 44100;
	int err;

	pw->sample_bits = snd_pcm_format_physical_width(format);
	if ((err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_ACCESS,
						 SPA_N_ELEMENTS(access_list), access_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_FORMAT,
						 1, &format)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_CHANNELS,
						   2, 2)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_RATE,
						   rate, rate)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
						SPA_N_ELEMENTS(bytes_list), bytes_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&pw->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
						SPA_N_ELEMENTS(bytes_list), bytes_list)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&pw->io, SND_PCM_IOPLUG_HW_PERIODS,
						   2, 64)) < 0)
		return err;

	return 0;
}

static int impl_send_command(struct spa_node *node, const struct spa_command *command)
{
	return 0;
}

static int impl_set_callbacks(struct spa_node *node,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	d->callbacks = callbacks;
	d->callbacks_data = data;
	return 0;
}

static int impl_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);

	pw_log_debug("%d %d", d->io.stream, SND_PCM_STREAM_PLAYBACK);
	if (d->io.stream == SND_PCM_STREAM_PLAYBACK) {
		*n_input_ports = *max_input_ports = 0;
		*n_output_ports = *max_output_ports = 1;
	}
	else {
		*n_input_ports = *max_input_ports = 1;
		*n_output_ports = *max_output_ports = 0;
	}
	return 0;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t *input_ids,
                             uint32_t n_input_ids,
                             uint32_t *output_ids,
                             uint32_t n_output_ids)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);

	pw_log_debug("%d %d", d->io.stream, SND_PCM_STREAM_PLAYBACK);
	if (d->io.stream == SND_PCM_STREAM_PLAYBACK) {
		if (n_output_ids > 0)
	                output_ids[0] = 0;
	}
	else {
		if (n_input_ids > 0)
	                input_ids[0] = 0;
	}
	return 0;
}

static int impl_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);

	if (id == d->t->io.Buffers)
		d->port_io = data;
	else
		return -ENOENT;

	return 0;
}

static int impl_port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			      const struct spa_port_info **info)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = NULL;

	*info = &d->port_info;

	return 0;
}

static int impl_port_enum_params(struct spa_node *node,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	struct pw_type *t = d->t;
	int bps;

	bps = d->io.channels * (d->sample_bits / 8);

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta };

		if (*index < SPA_N_ELEMENTS(list))
			*result = spa_pod_builder_object(builder,
				id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		if (*index != 0)
			return 0;

		*result = spa_pod_builder_object(builder,
			id, d->type.format,
			"I", d->type.media_type.audio,
			"I", d->type.media_subtype.raw,
			":", d->type.format_audio.format,   "I", d->type.audio_format.S16,
			":", d->type.format_audio.channels, "i", 2,
			":", d->type.format_audio.rate,     "i", 44100);
	}
	else if (id == t->param.idFormat) {
		if (*index != 0 || d->format.format == 0)
			return 0;

		*result = spa_pod_builder_object(builder,
			id, d->type.format,
			"I", d->type.media_type.audio,
			"I", d->type.media_subtype.raw,
			":", d->type.format_audio.format,   "I", d->format.format,
			":", d->type.format_audio.channels, "i", d->format.channels,
			":", d->type.format_audio.rate,     "i", d->format.rate);
	}
	else if (id == t->param.idBuffers) {
		if (*index != 0 || d->format.format == 0)
			return 0;

		*result = spa_pod_builder_object(builder,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "iru", d->io.buffer_size * bps,
									2, d->min_avail * bps, INT32_MAX / bps,
			":", t->param_buffers.stride,  "i",   0,
			":", t->param_buffers.buffers, "iru", 1,
									2, 1, 32,
			":", t->param_buffers.align,   "i",  16);
	}
	else if (id == t->param.idMeta) {
		if (d->format.format == 0)
			return 0;

		switch (*index) {
		case 0:
			*result = spa_pod_builder_object(builder,
				id, t->param_meta.Meta,
				":", t->param_meta.type, "I", t->meta.Header,
				":", t->param_meta.size, "i", sizeof(struct spa_meta_header));
			break;
		default:
	                return 0;
		}
	}
	else
		return -ENOENT;

	(*index)++;

        return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);

	if (format == NULL) {
		d->format.format = 0;
		return 0;
	}

	if (spa_format_audio_raw_parse(format, &d->format, &d->type.format_audio) < 0)
		return -EINVAL;

	if (d->format.format != d->type.audio_format.S16)
		return -EINVAL;

	return 0;
}

static int impl_port_set_param(struct spa_node *node,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	struct pw_type *t = d->t;

	if (id == t->param.idFormat) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int impl_port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				 struct spa_buffer **buffers, uint32_t n_buffers)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	uint32_t i;
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &d->buffers[i];
		struct spa_data *datas = buffers[i]->datas;

		if (datas[0].data != NULL) {
			b->ptr = datas[0].data;
			b->mapped = false;
		}
		else if (datas[0].type == d->t->data.MemFd ||
			 datas[0].type == d->t->data.DmaBuf) {
			b->ptr = mmap(NULL, datas[0].maxsize + datas[0].mapoffset, PROT_WRITE,
				      MAP_SHARED, datas[0].fd, 0);
			if (b->ptr == MAP_FAILED) {
				pw_log_error("failed to buffer mem");
				b->ptr = NULL;
				return -errno;

			}
			b->ptr = SPA_MEMBER(b->ptr, datas[0].mapoffset, void);
			b->mapped = true;
		}
		else {
			pw_log_error("invalid buffer mem");
			return -EINVAL;
		}
		b->buffer = buffers[i];
                b->h = spa_buffer_find_meta(b->buffer, d->t->meta.Header);

		pw_log_info("got buffer %d size %d", i, datas[0].maxsize);
		spa_list_append(&d->empty, &b->link);
		b->used = false;
	}
	d->n_buffers = n_buffers;
	return 0;
}

static inline void reuse_buffer(snd_pcm_pipewire_t *d, uint32_t id)
{
	struct buffer *b = &d->buffers[id];
	if (b->used) {
		pw_log_trace("reuse buffer %d", id);
	        spa_list_append(&d->empty, &b->link);
		b->used = false;
	}
}

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	reuse_buffer(d, buffer_id);
	return 0;
}

static int impl_node_process_input(struct spa_node *node)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
        struct spa_io_buffers *io = d->port_io;
	struct buffer *b;

	if (io->buffer_id >= d->n_buffers) {
		io->status = -EINVAL;
		return -EINVAL;
	}
	b = &d->buffers[io->buffer_id];

	snd_pcm_pipewire_process_record(d, b);

	return io->status = SPA_STATUS_NEED_BUFFER;
}

static int impl_node_process_output(struct spa_node *node)
{
	snd_pcm_pipewire_t *d = SPA_CONTAINER_OF(node, snd_pcm_pipewire_t, impl_node);
	struct buffer *b;
        struct spa_io_buffers *io = d->port_io;

	if (io->buffer_id < d->n_buffers) {
		reuse_buffer(d, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}
	if (spa_list_is_empty(&d->empty)) {
                pw_log_error("alsa-pipewire %p: out of buffers", d);
                return -EPIPE;
        }
        b = spa_list_first(&d->empty, struct buffer, link);
        spa_list_remove(&b->link);

	b->used = true;
	io->buffer_id = b->buffer->id;

	snd_pcm_pipewire_process_playback(d, b);

	return io->status = SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.send_command = impl_send_command,
	.set_callbacks = impl_set_callbacks,
	.get_n_ports = impl_get_n_ports,
	.get_port_ids = impl_get_port_ids,
	.port_set_io = impl_port_set_io,
	.port_get_info = impl_port_get_info,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_reuse_buffer = impl_port_reuse_buffer,
	.process_output = impl_node_process_output,
	.process_input = impl_node_process_input,
};


static void on_state_changed(void *data, enum pw_remote_state old,
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
        .state_changed = on_state_changed,
};

static int pipewire_node_create(snd_pcm_pipewire_t *pw,
				const char *node_name,
				const char *target,
				bool can_fallback)
{
	const char *error = NULL;
	enum pw_remote_state state;

	pw_thread_loop_lock(pw->main_loop);

	if (pw_remote_connect(pw->remote) < 0) {
		error = "connect failed";
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
	pw_log_debug("node %s", target);
        pw->node = pw_node_new(pw->core,
			       node_name,
			       pw_properties_new(PW_NODE_PROP_AUTOCONNECT, "1",
						 PW_NODE_PROP_TARGET_NODE, target,
						 NULL),
			       0);
	pw->impl_node = impl_node;
        pw_node_set_implementation(pw->node, &pw->impl_node);
        pw_node_register(pw->node, NULL, NULL, NULL);

        pw_remote_export(pw->remote, pw->node);
	pw_thread_loop_unlock(pw->main_loop);

	return 0;

      error:
        if (!can_fallback)
                SNDERR("PipeWire: Unable to connect: %s\n", error);

        pw_thread_loop_unlock(pw->main_loop);

        return -ECONNREFUSED;

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
	char pipewire_node_name[32];
	const char *target;

	assert(pcmp);
	pw = calloc(1, sizeof(*pw));
	if (!pw)
		return -ENOMEM;

	pw_log_debug("open %s %d %d", name, stream, mode);

	pw->fd = -1;
	pw->io.poll_fd = -1;
	spa_list_init(&pw->empty);

	if (node_name == NULL)
		err = snprintf(pipewire_node_name, sizeof(pipewire_node_name),
			       "alsa-pipewire.%s%s.%d.%d", name,
			       stream == SND_PCM_STREAM_PLAYBACK ? "P" : "C",
			       getpid(), num++);
	else
		err = snprintf(pipewire_node_name, sizeof(pipewire_node_name),
			       "%s", node_name);

	if (stream == SND_PCM_STREAM_PLAYBACK)
		target = playback_node;
	else
		target = capture_node;

        pw->loop = pw_loop_new(NULL);
        pw->main_loop = pw_thread_loop_new(pw->loop, "alsa-pipewire");
        pw->core = pw_core_new(pw->loop, NULL);
        pw->t = pw_core_get_type(pw->core);
        init_type(&pw->type, pw->t->map);
        spa_debug_set_type_map(pw->t->map);

        pw->remote = pw_remote_new(pw->core, NULL, 0);
        pw_remote_add_listener(pw->remote, &pw->remote_listener, &remote_events, pw);

	err = pw_thread_loop_start(pw->main_loop);
	if (err < 0) {
		snd_pcm_pipewire_free(pw);
		return err;
	}

	pw->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	pw->io.version = SND_PCM_IOPLUG_VERSION;
	pw->io.name = "ALSA <-> PipeWire PCM I/O Plugin";
	pw->io.callback = &pipewire_pcm_callback;
	pw->io.private_data = pw;
	pw->io.poll_fd = pw->fd;
	pw->io.poll_events = POLLIN;
	pw->io.mmap_rw = 1;

	err = snd_pcm_ioplug_create(&pw->io, name, stream, mode);
	if (err < 0) {
		snd_pcm_pipewire_free(pw);
		return err;
	}
	pw_log_debug("open %s %d %d", name, pw->io.stream, mode);

	err = pipewire_set_hw_constraint(pw);
	if (err < 0) {
		snd_pcm_ioplug_delete(&pw->io);
		return err;
	}

	err = pipewire_node_create(pw, pipewire_node_name, target, false);
	if (err < 0) {
		snd_pcm_pipewire_free(pw);
		return err;
	}

	*pcmp = pw->io.pcm;

	return 0;
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
