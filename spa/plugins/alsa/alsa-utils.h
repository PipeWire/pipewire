/* Spa ALSA Sink
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_ALSA_UTILS_H__
#define __SPA_ALSA_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include <asoundlib.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/list.h>

#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/audio/format-utils.h>

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2

struct props {
	char device[64];
	char device_name[128];
	char card_name[128];
	uint32_t min_latency;
	uint32_t max_latency;
};

#define MAX_BUFFERS 32

struct buffer {
	struct spa_buffer *buf;
	struct spa_meta_header *h;
#define BUFFER_FLAG_OUT	(1<<0)
	uint32_t flags;
	struct spa_list link;
};

struct state {
	struct spa_handle handle;
	struct spa_node node;

	uint32_t seq;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	snd_pcm_stream_t stream;
	snd_output_t *output;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct props props;

	bool opened;
	snd_pcm_t *hndl;

	bool have_format;
	struct spa_audio_info current_format;

	snd_pcm_uframes_t buffer_frames;
	snd_pcm_uframes_t period_frames;
	snd_pcm_format_t format;
	int rate;
	int channels;
	size_t frame_size;

	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_range *range;
	struct spa_io_clock *clock;

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;

	struct spa_list free;
	struct spa_list ready;

	size_t ready_offset;

	bool started;
	struct spa_source source;
	int timerfd;
	bool alsa_started;
	int threshold;

	snd_htimestamp_t now;
	int64_t sample_count;
	int64_t filled;

	uint64_t underrun;
};

int
spa_alsa_enum_format(struct state *state,
		     uint32_t *index,
		     const struct spa_pod *filter,
		     struct spa_pod **result,
		     struct spa_pod_builder *builder);

int spa_alsa_set_format(struct state *state, struct spa_audio_info *info, uint32_t flags);

int spa_alsa_start(struct state *state, bool xrun_recover);
int spa_alsa_pause(struct state *state, bool xrun_recover);
int spa_alsa_close(struct state *state);

int spa_alsa_write(struct state *state, snd_pcm_uframes_t silence);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_ALSA_UTILS_H__ */
