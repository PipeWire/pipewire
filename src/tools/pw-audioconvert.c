/* PipeWire - pw-filter-graph */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>

#include <sndfile.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/layout-types.h>
#include <spa/param/audio/raw-json.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/debug/file.h>

#include <pipewire/pipewire.h>

#define MAX_SAMPLES	4096u

enum {
	OPT_CHANNELMAP = 1000,
};

struct data {
	bool verbose;
	int rate;
	int format;
	uint32_t blocksize;
	int out_channels;
	const char *channel_map;
	struct pw_properties *props;

	const char *iname;
	SF_INFO iinfo;
	SNDFILE *ifile;

	const char *oname;
	SF_INFO oinfo;
	SNDFILE *ofile;

	struct pw_main_loop *loop;
	struct pw_context *context;

	struct spa_handle *handle;
	struct spa_node *node;
};

#define STR_FMTS "(s8|s16|s32|f32|f64)"

#define OPTIONS		"hvr:f:b:P:c:"
static const struct option long_options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "verbose",	no_argument,		NULL, 'v' },

	{ "rate",	required_argument,	NULL, 'r' },
	{ "format",	required_argument,	NULL, 'f' },
	{ "blocksize",	required_argument,	NULL, 'b' },
	{ "properties",	required_argument,	NULL, 'P' },
	{ "channels",	required_argument,	NULL, 'c' },
	{ "channel-map", required_argument,	NULL, OPT_CHANNELMAP },

	{ NULL, 0, NULL, 0 }
};

static void show_usage(const char *name, bool is_error)
{
	FILE *fp;

	fp = is_error ? stderr : stdout;

	fprintf(fp, "%s [options] <infile> <outfile>\n", name);
	fprintf(fp,
		"  -h, --help                            Show this help\n"
		"  -v  --verbose                         Be verbose\n"
		"\n");
	fprintf(fp,
		"  -r  --rate                            Output sample rate (default as input)\n"
		"  -f  --format                          Output sample format %s (default as input)\n"
		"  -b  --blocksize                       Number of samples per iteration (default %u)\n"
		"  -P  --properties                      Set node properties (optional)\n"
		"                                        Use @filename to read from file\n"
		"  -c  --channels                        Output channel count\n"
		"      --channel-map                     Output channel layout (e.g. \"stereo\", \"5.1\",\n"
		"                                        \"FL,FR,FC,LFE,SL,SR\")\n",
		STR_FMTS, MAX_SAMPLES);
	fprintf(fp, "\n");
}

static inline const char *
sf_fmt_to_str(int fmt)
{
	switch(fmt & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_S8:
		return "s8";
	case SF_FORMAT_PCM_16:
		return "s16";
	case SF_FORMAT_PCM_24:
		return "s24";
	case SF_FORMAT_PCM_32:
		return "s32";
	case SF_FORMAT_FLOAT:
		return "f32";
	case SF_FORMAT_DOUBLE:
		return "f64";
	default:
		return "unknown";
	}
}

static inline int
sf_str_to_fmt(const char *str)
{
	if (!str)
		return -1;
	if (spa_streq(str, "s8"))
		return SF_FORMAT_PCM_S8;
	if (spa_streq(str, "s16"))
		return SF_FORMAT_PCM_16;
	if (spa_streq(str, "s24"))
		return SF_FORMAT_PCM_24;
	if (spa_streq(str, "s32"))
		return SF_FORMAT_PCM_32;
	if (spa_streq(str, "f32"))
		return SF_FORMAT_FLOAT;
	if (spa_streq(str, "f64"))
		return SF_FORMAT_DOUBLE;
	return -1;
}

static int parse_channelmap(const char *channel_map, struct spa_audio_layout_info *map)
{
	if (spa_audio_layout_info_parse_name(map, sizeof(*map), channel_map) >= 0)
		return 0;

	spa_audio_parse_position_n(channel_map, strlen(channel_map),
			map->position, SPA_N_ELEMENTS(map->position), &map->n_channels);
	return map->n_channels > 0 ? 0 : -EINVAL;
}

static int channelmap_default(struct spa_audio_layout_info *map, int n_channels)
{
	switch(n_channels) {
	case 1: parse_channelmap("Mono", map); break;
	case 2: parse_channelmap("Stereo", map); break;
	case 3: parse_channelmap("2.1", map); break;
	case 4: parse_channelmap("Quad", map); break;
	case 5: parse_channelmap("5.0", map); break;
	case 6: parse_channelmap("5.1", map); break;
	case 7: parse_channelmap("7.0", map); break;
	case 8: parse_channelmap("7.1", map); break;
	default: n_channels = 0; break;
	}
	map->n_channels = n_channels;
	return 0;
}

static int open_input(struct data *d)
{
	d->ifile = sf_open(d->iname, SFM_READ, &d->iinfo);
	if (d->ifile == NULL) {
		fprintf(stderr, "error: failed to open input file \"%s\": %s\n",
				d->iname, sf_strerror(NULL));
		return -EIO;
	}
	if (d->verbose)
		fprintf(stdout, "input '%s': channels:%d rate:%d format:%s\n",
				d->iname, d->iinfo.channels, d->iinfo.samplerate,
				sf_fmt_to_str(d->iinfo.format));
	return 0;
}

static int open_output(struct data *d, int channels)
{
	int i, count = 0, format = -1;

	d->oinfo.channels = channels;
	d->oinfo.samplerate = d->rate > 0 ? d->rate : d->iinfo.samplerate;
	d->oinfo.format = d->format > 0 ? d->format : (d->iinfo.format & SF_FORMAT_SUBMASK);

	/* try to guess the format from the extension */
	if (sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof(int)) != 0)
		count = 0;

	for (i = 0; i < count; i++) {
		SF_FORMAT_INFO fi;

		spa_zero(fi);
		fi.format = i;
		if (sf_command(NULL, SFC_GET_FORMAT_MAJOR, &fi, sizeof(fi)) != 0)
			continue;

		if (spa_strendswith(d->oname, fi.extension)) {
			format = fi.format;
			break;
		}
	}
	if (format == -1)
		format = d->iinfo.format & ~SF_FORMAT_SUBMASK;
	if (format == SF_FORMAT_WAV && d->oinfo.channels > 2)
		format = SF_FORMAT_WAVEX;

	d->oinfo.format |= format;

	d->ofile = sf_open(d->oname, SFM_WRITE, &d->oinfo);
	if (d->ofile == NULL) {
		fprintf(stderr, "error: failed to open output file \"%s\": %s\n",
				d->oname, sf_strerror(NULL));
		return -EIO;
	}
	sf_command(d->ofile, SFC_SET_CLIPPING, NULL, 1);

	if (d->verbose)
		fprintf(stdout, "output '%s': channels:%d rate:%d format:%s\n",
				d->oname, d->oinfo.channels, d->oinfo.samplerate,
				sf_fmt_to_str(d->oinfo.format));
	return 0;
}

static int setup_convert_direction(struct spa_node *node,
		enum spa_direction direction, struct spa_audio_info_raw *info)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param, *format;
	int res;

	/* set port config to convert mode */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,  SPA_POD_Id(direction),
		SPA_PARAM_PORT_CONFIG_mode,       SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_convert));

	res = spa_node_set_param(node, SPA_PARAM_PortConfig, 0, param);
	if (res < 0)
		return res;

	/* set format on port 0 */
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_format_audio_raw_build(&b, SPA_PARAM_Format, info);

	res = spa_node_port_set_param(node, direction, 0,
			SPA_PARAM_Format, 0, format);
	return res;
}

static int do_filter(struct data *d)
{
	void *iface;
	struct spa_node *node;
	int in_channels = d->iinfo.channels;
	int out_channels;
	int in_rate = d->iinfo.samplerate;
	int out_rate = d->rate > 0 ? d->rate : in_rate;
	uint32_t blocksize = d->blocksize > 0 ? d->blocksize : MAX_SAMPLES;
	int res;
	struct spa_audio_info_raw in_info, out_info;
	struct spa_audio_layout_info in_layout, out_layout;

	/* determine output channels */
	out_channels = d->out_channels > 0 ? d->out_channels : in_channels;

	/* set up input channel layout */
	spa_zero(in_layout);
	channelmap_default(&in_layout, in_channels);

	/* set up output channel layout */
	spa_zero(out_layout);
	if (d->channel_map != NULL) {
		if (parse_channelmap(d->channel_map, &out_layout) < 0) {
			fprintf(stderr, "error: can't parse channel-map '%s'\n",
					d->channel_map);
			return -EINVAL;
		}
		if (d->out_channels > 0 &&
		    out_layout.n_channels != (uint32_t)d->out_channels) {
			fprintf(stderr, "error: channel-map has %u channels "
					"but -c specifies %d\n",
					out_layout.n_channels, d->out_channels);
			return -EINVAL;
		}
		out_channels = out_layout.n_channels;
	} else {
		channelmap_default(&out_layout, out_channels);
	}

	/* open the output file */
	res = open_output(d, out_channels);
	if (res < 0)
		return res;

	/* calculate output buffer size accounting for resampling */
	uint32_t out_blocksize = (uint32_t)((uint64_t)blocksize *
			out_rate / in_rate) + 64;

	uint32_t quant_limit = SPA_ROUND_UP_N(SPA_MAX(out_blocksize, blocksize), 4096);

	pw_properties_setf(d->props, "clock.quantum-limit", "%u", quant_limit);
	pw_properties_set(d->props, "convert.direction", "output");

	d->handle = pw_context_load_spa_handle(d->context,
			SPA_NAME_AUDIO_CONVERT, &d->props->dict);

	if (d->handle == NULL) {
		fprintf(stderr, "can't load %s: %m\n", SPA_NAME_AUDIO_CONVERT);
		return -errno;
	}

	res = spa_handle_get_interface(d->handle,
			SPA_TYPE_INTERFACE_Node, &iface);
	if (res < 0 || iface == NULL) {
		fprintf(stderr, "can't get Node interface: %s\n",
				spa_strerror(res));
		return res;
	}
	node = d->node = iface;

	/* build input format: interleaved F32 */
	spa_zero(in_info);
	in_info.format = SPA_AUDIO_FORMAT_F32;
	in_info.rate = in_rate;
	in_info.channels = in_channels;
	for (uint32_t i = 0; i < in_layout.n_channels &&
			i < SPA_AUDIO_MAX_CHANNELS; i++)
		in_info.position[i] = in_layout.position[i];

	/* build output format: interleaved F32 */
	spa_zero(out_info);
	out_info.format = SPA_AUDIO_FORMAT_F32;
	out_info.rate = out_rate;
	out_info.channels = out_channels;
	for (uint32_t i = 0; i < out_layout.n_channels &&
			i < SPA_AUDIO_MAX_CHANNELS; i++)
		out_info.position[i] = out_layout.position[i];

	/* set up convert directions */
	res = setup_convert_direction(node, SPA_DIRECTION_INPUT, &in_info);
	if (res < 0) {
		fprintf(stderr, "can't set input format: %s\n",
				spa_strerror(res));
		return res;
	}
	res = setup_convert_direction(node, SPA_DIRECTION_OUTPUT, &out_info);
	if (res < 0) {
		fprintf(stderr, "can't set output format: %s\n",
				spa_strerror(res));
		return res;
	}

	/* send Start command */
	{
		struct spa_command cmd =
			SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
		res = spa_node_send_command(node, &cmd);
		if (res < 0) {
			fprintf(stderr, "can't start node: %s\n",
					spa_strerror(res));
			return res;
		}
	}

	if (d->verbose)
		fprintf(stdout, "convert: in:%dch@%dHz -> out:%dch@%dHz "
				"blocksize:%u\n",
				in_channels, in_rate,
				out_channels, out_rate, blocksize);

	/* process audio */
	{
		float ibuf[blocksize * in_channels] SPA_ALIGNED(64);
		float obuf[out_blocksize * out_channels] SPA_ALIGNED(64);

		struct spa_chunk in_chunk, out_chunk;
		struct spa_data in_sdata, out_sdata;
		struct spa_buffer in_buffer, out_buffer;
		struct spa_buffer *in_buffers[1] = { &in_buffer };
		struct spa_buffer *out_buffers[1] = { &out_buffer };
		struct spa_io_buffers in_io, out_io;
		size_t read_total = 0, written_total = 0;

		/* setup input buffer */
		spa_zero(in_chunk);
		spa_zero(in_sdata);
		in_sdata.type = SPA_DATA_MemPtr;
		in_sdata.flags = SPA_DATA_FLAG_READABLE;
		in_sdata.fd = -1;
		in_sdata.maxsize = sizeof(ibuf);
		in_sdata.data = ibuf;
		in_sdata.chunk = &in_chunk;

		spa_zero(in_buffer);
		in_buffer.datas = &in_sdata;
		in_buffer.n_datas = 1;

		res = spa_node_port_use_buffers(node,
				SPA_DIRECTION_INPUT, 0, 0,
				in_buffers, 1);
		if (res < 0) {
			fprintf(stderr, "can't set input buffers: %s\n",
					spa_strerror(res));
			goto stop;
		}

		/* setup output buffer */
		spa_zero(out_chunk);
		spa_zero(out_sdata);
		out_sdata.type = SPA_DATA_MemPtr;
		out_sdata.flags = SPA_DATA_FLAG_READWRITE;
		out_sdata.fd = -1;
		out_sdata.maxsize = sizeof(obuf);
		out_sdata.data = obuf;
		out_sdata.chunk = &out_chunk;

		spa_zero(out_buffer);
		out_buffer.datas = &out_sdata;
		out_buffer.n_datas = 1;

		res = spa_node_port_use_buffers(node,
				SPA_DIRECTION_OUTPUT, 0, 0,
				out_buffers, 1);
		if (res < 0) {
			fprintf(stderr, "can't set output buffers: %s\n",
					spa_strerror(res));
			goto stop;
		}

		/* setup IO */
		res = spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
				SPA_IO_Buffers, &in_io, sizeof(in_io));
		if (res < 0) {
			fprintf(stderr, "can't set input IO: %s\n",
					spa_strerror(res));
			goto stop;
		}
		res = spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0,
				SPA_IO_Buffers, &out_io, sizeof(out_io));
		if (res < 0) {
			fprintf(stderr, "can't set output IO: %s\n",
					spa_strerror(res));
			goto stop;
		}

		/* process loop */
		while (true) {
			sf_count_t n_read;

			n_read = sf_readf_float(d->ifile, ibuf, blocksize);

			read_total += n_read;

			in_chunk.offset = 0;
			in_chunk.size = n_read * in_channels * sizeof(float);
			in_chunk.stride = 0;

			out_chunk.offset = 0;
			out_chunk.size = 0;
			out_chunk.stride = 0;

			in_io.status = n_read > 0 ? SPA_STATUS_HAVE_DATA : SPA_STATUS_DRAINED;
			in_io.buffer_id = 0;
			out_io.status = SPA_STATUS_NEED_DATA;
			out_io.buffer_id = 0;

			res = spa_node_process(node);
			if (res < 0) {
				fprintf(stderr, "process error: %s\n",
						spa_strerror(res));
				break;
			}

			if (out_io.status == SPA_STATUS_HAVE_DATA &&
			    out_io.buffer_id == 0) {
				uint32_t out_frames = out_chunk.size /
					(out_channels * sizeof(float));
				if (out_frames > 0)
					written_total += sf_writef_float(
							d->ofile, obuf,
							out_frames);
			}
			if (n_read == 0)
				break;
		}
		if (d->verbose)
			fprintf(stdout, "read %zu samples, wrote %zu samples\n",
					read_total, written_total);
	}

	res = 0;

stop:
	{
		struct spa_command cmd =
			SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);
		spa_node_send_command(node, &cmd);
	}
	return res;
}

static char *read_file(const char *path)
{
	FILE *f;
	long size;
	char *buf;

	f = fopen(path, "r");
	if (f == NULL)
		return NULL;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = malloc(size + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}

	if ((long)fread(buf, 1, size, f) != size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[size] = '\0';
	fclose(f);
	return buf;
}

int main(int argc, char *argv[])
{
	int c;
	int longopt_index = 0, ret;
	struct data data;
	struct spa_error_location loc;
	char *file_content = NULL, *str;

	spa_zero(data);
	data.props = pw_properties_new(NULL, NULL);

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, OPTIONS, long_options, &longopt_index)) != -1) {
		switch (c) {
		case 'h':
			show_usage(argv[0], false);
			ret = EXIT_SUCCESS;
			goto done;
		case 'v':
			data.verbose = true;
			break;
		case 'r':
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad rate %s\n", optarg);
				goto error_usage;
			}
			data.rate = ret;
			break;
		case 'f':
			ret = sf_str_to_fmt(optarg);
			if (ret < 0) {
				fprintf(stderr, "error: bad format %s\n", optarg);
				goto error_usage;
			}
			data.format = ret;
			break;
		case 'b':
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad blocksize %s\n", optarg);
				goto error_usage;
			}
			data.blocksize = ret;
			break;
		case 'P':
			if (optarg[0] == '@') {
				file_content = read_file(optarg + 1);
				if (file_content == NULL) {
					fprintf(stderr, "error: can't read graph file '%s': %m\n",
							optarg + 1);
					ret = EXIT_FAILURE;
					goto done;
				}
				str = file_content;
			} else {
				str = optarg;
			}
			if (pw_properties_update_string_checked(data.props, str, strlen(str), &loc) < 0) {
				spa_debug_file_error_location(stderr, &loc,
						"error: syntax error in --properties: %s",
						loc.reason);
				goto error_usage;
			}
			break;
		case 'c':
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad channel count %s\n", optarg);
				goto error_usage;
			}
			data.out_channels = ret;
			break;
		case OPT_CHANNELMAP:
			data.channel_map = optarg;
			break;
		default:
			fprintf(stderr, "error: unknown option '%c'\n", c);
			goto error_usage;
		}
	}
	if (optind + 1 >= argc) {
		fprintf(stderr, "error: filename arguments missing\n");
		goto error_usage;
	}
	data.iname = argv[optind++];
	data.oname = argv[optind++];

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "error: can't create main loop: %m\n");
		ret = EXIT_FAILURE;
		goto done;
	}

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "error: can't create context: %m\n");
		ret = EXIT_FAILURE;
		goto done;
	}

	if (open_input(&data) < 0) {
		ret = EXIT_FAILURE;
		goto done;
	}

	ret = do_filter(&data);

done:
	if (data.ifile)
		sf_close(data.ifile);
	if (data.ofile)
		sf_close(data.ofile);
	if (data.props)
		pw_properties_free(data.props);
	if (data.handle)
		pw_unload_spa_handle(data.handle);
	if (data.context)
		pw_context_destroy(data.context);
	if (data.loop)
		pw_main_loop_destroy(data.loop);
	free(file_content);
	pw_deinit();

	return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

error_usage:
	show_usage(argv[0], true);
	ret = EXIT_FAILURE;
	goto done;
}
