/* Spa ALSA Sink
 *
 * Copyright © 2018 Wim Taymans
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
	struct spa_io_position *position;

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
