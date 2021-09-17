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
#include <spa/param/audio/layout.h>

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

static const uint8_t bitrev[256] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

ssize_t
dsf_file_read(struct dsf_file *f, void *data, size_t samples, uint32_t interleave, bool lsb)
{
	uint8_t *d = data;
	const uint8_t *s;
	uint32_t i, j, k, total = 0, size, stride, bytes;
	bool rev = lsb != f->info.lsb;

	stride = f->info.channels * interleave;
	size = f->info.channels * f->info.blocksize;

	bytes = samples * stride;
	bytes -= bytes % f->info.blocksize;

	s = f->p;
	while (total < bytes) {
		for (i = 0; i < f->info.blocksize; i += interleave)  {
			for (j = 0; j < f->info.channels; j++) {
				const uint8_t *c = &s[f->info.blocksize * j + i];
				for (k = 0; k < interleave; k++)
					*d++ = rev ? bitrev[c[k]] : c[k];
			}
		}
		s += size;
		total += size;
	}
	f->p += total;
	return total / stride;
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

	struct spa_audio_info_dsd format;
};

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	ssize_t stride;
	uint32_t samples;
	uint8_t *d;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((d = buf->datas[0].data) == NULL)
		return;

	stride = data->info.channels * data->format.interleave;

	samples = dsf_file_read(data->f, d, buf->datas[0].maxsize / stride,
			data->format.interleave,
			data->format.bitorder == SPA_PARAM_BITORDER_lsb);

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = samples * stride;

	pw_stream_queue_buffer(data->stream, b);
}

static void
on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct data *data = userdata;
	struct spa_audio_info info = { 0 };
	int err;

	if (id != SPA_PARAM_Format || param == NULL)
		return;

	if ((err = spa_format_parse(param, &info.media_type, &info.media_subtype)) < 0)
		return;

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_dsd)
		return;

	if (spa_format_audio_dsd_parse(param, &info.info.dsd) < 0)
		return;

	data->format = info.info.dsd;
	fprintf(stderr, "output:\n");
	fprintf(stderr, " bitorder: %s\n",
			data->format.bitorder == SPA_PARAM_BITORDER_lsb ? "lsb" : "msb");
	fprintf(stderr, " interleave: %d\n", data->format.interleave);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

struct layout_info {
	 uint32_t type;
	 struct spa_audio_layout_info info;
};

static const struct layout_info layouts[] = {
	{ 1, { SPA_AUDIO_LAYOUT_Mono, }, },
	{ 2, { SPA_AUDIO_LAYOUT_Stereo, }, },
	{ 3, { SPA_AUDIO_LAYOUT_2FC }, },
	{ 4, { SPA_AUDIO_LAYOUT_Quad }, },
	{ 5, { SPA_AUDIO_LAYOUT_3_1 }, },
	{ 6, { SPA_AUDIO_LAYOUT_5_0R }, },
	{ 7, { SPA_AUDIO_LAYOUT_5_1R }, },
};

int handle_dsd_playback(struct data *data)
{
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_dsd info;
	size_t i;

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
	info.channels = data->info.channels;
	info.rate = data->info.rate / 8;

	for (i = 0; i < SPA_N_ELEMENTS(layouts); i++) {
		if (layouts[i].type != data->info.channel_type)
			continue;
		info.channels = layouts[i].info.n_channels;
		memcpy(info.position, layouts[i].info.position,
				info.channels * sizeof(uint32_t));
	}

	params[0] = spa_format_audio_dsd_build(&b, SPA_PARAM_EnumFormat, &info);

	/* Now connect this stream. We ask that our process function is
	 * called in a realtime thread. */
	pw_stream_connect(data->stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS,
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
