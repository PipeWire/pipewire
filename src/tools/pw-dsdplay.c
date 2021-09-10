/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

struct dsf_file_info {
	uint32_t channel_type;
	uint32_t channels;
	uint32_t rate;
	bool lsb;
	uint64_t samples;
	uint64_t length;
	uint32_t blocksize;
};

struct dsf_file {
	uint8_t *data;
	size_t size;

	int mode;
	int fd;

	struct dsf_file_info info;

	uint8_t *p;
};

static inline uint16_t parse_le16(const uint8_t *in)
{
	return in[0] | (in[1] << 8);
}

static inline uint32_t parse_le32(const uint8_t *in)
{
	return in[0] | (in[1] << 8) | (in[2] << 16) | (in[3] << 24);
}

static inline uint64_t parse_le64(const uint8_t *in)
{
	uint64_t res = in[0];
	res |= ((uint64_t)in[1]) << 8;
	res |= ((uint64_t)in[2]) << 16;
	res |= ((uint64_t)in[3]) << 24;
	res |= ((uint64_t)in[4]) << 32;
	res |= ((uint64_t)in[5]) << 40;
	res |= ((uint64_t)in[6]) << 48;
	res |= ((uint64_t)in[7]) << 56;
	return res;
}

static inline int f_avail(struct dsf_file *f)
{
	if (f->p < f->data + f->size)
		return f->size + f->data - f->p;
	return 0;
}

static int read_DSD(struct dsf_file *f)
{
	uint64_t size;

	if (f_avail(f) < 28 ||
	    memcmp(f->p, "DSD ", 4) != 0)
		return -EINVAL;

	size = parse_le64(f->p + 4);	/* size of this chunk */
	parse_le64(f->p + 12);		/* total size */
	parse_le64(f->p + 20);		/* metadata */
	f->p += size;
	return 0;
}

static int read_fmt(struct dsf_file *f)
{
	uint64_t size;

	if (f_avail(f) < 52 ||
	    memcmp(f->p, "fmt ", 4) != 0)
		return -EINVAL;

	size = parse_le64(f->p + 4);	/* size of this chunk */
	if (parse_le32(f->p + 12) != 1)	/* version */
		return -EINVAL;
	if (parse_le32(f->p + 16) != 0)	/* format id */
		return -EINVAL;

	f->info.channel_type = parse_le32(f->p + 20);
	f->info.channels = parse_le32(f->p + 24);
	f->info.rate = parse_le32(f->p + 28);
	f->info.lsb = parse_le32(f->p + 32) == 1;
	f->info.samples = parse_le64(f->p + 36);
	f->info.blocksize = parse_le32(f->p + 44);
	f->p += size;
	return 0;
}

static int read_data(struct dsf_file *f)
{
	uint64_t size;

	if (f_avail(f) < 12 ||
	    memcmp(f->p, "data", 4) != 0)
		return -EINVAL;

	size = parse_le64(f->p + 4);	/* size of this chunk */
	f->info.length = size - 12;
	f->p += 12;
	return 0;
}

static int open_read(struct dsf_file *f, const char *filename, struct dsf_file_info *info)
{
	int res;
	struct stat st;

	if ((f->fd = open(filename, O_RDONLY)) < 0) {
		res = -errno;
		goto exit;
	}
	if (fstat(f->fd, &st) < 0) {
		res = -errno;
		goto exit_close;
	}
	f->size = st.st_size;

	f->data = mmap(NULL, f->size, PROT_READ, MAP_SHARED, f->fd, 0);
	if (f->data == MAP_FAILED) {
		res = -errno;
		goto exit_close;
	}

	f->p = f->data;

	if ((res = read_DSD(f)) < 0)
		goto exit_unmap;
	if ((res = read_fmt(f)) < 0)
		goto exit_unmap;
	if ((res = read_data(f)) < 0)
		goto exit_unmap;

	f->mode = 1;
	*info = f->info;
	return 0;

exit_unmap:
	munmap(f->data, f->size);
exit_close:
	close(f->fd);
exit:
	return res;
}

struct dsf_file *
dsf_file_open(const char *filename, const char *mode, struct dsf_file_info *info)
{
        int res;
        struct dsf_file *f;

        f = calloc(1, sizeof(struct dsf_file));
        if (f == NULL)
                return NULL;

        if (spa_streq(mode, "r")) {
                if ((res = open_read(f, filename, info)) < 0)
                        goto exit_free;
        } else {
                res = -EINVAL;
                goto exit_free;
        }
        return f;

exit_free:
        free(f);
        errno = -res;
        return NULL;
}

int dsf_file_close(struct dsf_file *f)
{
	if (f->mode == 1) {
		munmap(f->data, f->size);
	} else
		return -EINVAL;

	close(f->fd);
	free(f);
	return 0;
}

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;

	const char *opt_filename;
	const char *opt_remote;

	struct dsf_file *f;
	struct dsf_file_info info;
};

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int n_frames, stride;
	uint8_t *p;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	stride = data->info.channels * data->info.blocksize;

	n_frames = buf->datas[0].maxsize / stride;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n_frames * stride;

	pw_stream_queue_buffer(data->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int handle_dsd_playback(struct data *data)
{
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_dsd info;

	data->loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGINT, do_quit, data);
	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGTERM, do_quit, data);

	data->stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data->loop),
			"audio-src",
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data->opt_remote,
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, "Playback",
				PW_KEY_MEDIA_ROLE, "Music",
				NULL),
			&stream_events,
			data);

	spa_zero(info);
	info.bitorder = data->info.lsb ?  SPA_PARAM_BITORDER_lsb : SPA_PARAM_BITORDER_msb;
	info.channels = data->info.channels;
	info.rate = data->info.rate;

	switch (data->info.channel_type) {
	case 1:
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
		break;
	case 3:
		info.position[2] = SPA_AUDIO_CHANNEL_FC;
		SPA_FALLTHROUGH;
	case 2:
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	case 4:
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
		info.position[2] = SPA_AUDIO_CHANNEL_RL;
		info.position[3] = SPA_AUDIO_CHANNEL_RR;
		break;
	case 5:
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
		info.position[2] = SPA_AUDIO_CHANNEL_FC;
		info.position[3] = SPA_AUDIO_CHANNEL_LFE;
		break;
	case 6:
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
		info.position[2] = SPA_AUDIO_CHANNEL_FC;
		info.position[3] = SPA_AUDIO_CHANNEL_RL;
		info.position[4] = SPA_AUDIO_CHANNEL_RR;
		break;
	case 7:
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
		info.position[2] = SPA_AUDIO_CHANNEL_FC;
		info.position[3] = SPA_AUDIO_CHANNEL_LFE;
		info.position[4] = SPA_AUDIO_CHANNEL_RL;
		info.position[5] = SPA_AUDIO_CHANNEL_RR;
		break;
	default:
		break;
	}

	params[0] = spa_format_audio_dsd_build(&b, SPA_PARAM_EnumFormat, &info);

	/* Now connect this stream. We ask that our process function is
	 * called in a realtime thread. */
	pw_stream_connect(data->stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS |
			  PW_STREAM_FLAG_RT_PROCESS,
			  params, 1);

	/* and wait while we let things run */
	pw_main_loop_run(data->loop);

	pw_stream_destroy(data->stream);
	pw_main_loop_destroy(data->loop);
	return 0;
}

static void show_help(const char *name)
{
        fprintf(stdout, "%s [options] FILE\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	int c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ NULL,	0, NULL, 0}
	};

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVr:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			data.opt_remote = optarg;
			break;
		default:
			show_help(argv[0]);
			return -1;
		}
	}
	if (optind < argc)
		data.opt_filename = argv[optind];


	data.f = dsf_file_open(data.opt_filename, "r", &data.info);
	if (data.f == NULL) {
		fprintf(stderr, "can't open file %s: %m", data.opt_filename);
		return -1;
	}
	fprintf(stderr, "file details:\n");
	fprintf(stderr, " channel_type: %u\n", data.info.channel_type);
	fprintf(stderr, " channels: %u\n", data.info.channels);
	fprintf(stderr, " rate: %u\n", data.info.rate);
	fprintf(stderr, " lsb: %u\n", data.info.lsb);
	fprintf(stderr, " samples: %"PRIu64"\n", data.info.samples);
	fprintf(stderr, " length: %"PRIu64"\n", data.info.length);
	fprintf(stderr, " blocksize: %u\n", data.info.blocksize);

	handle_dsd_playback(&data);

	dsf_file_close(data.f);

	pw_deinit();

	return 0;
}
