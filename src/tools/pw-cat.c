/* PipeWire - pw-cat
 *
 * Copyright © 2020 Konsulko Group

 * Author: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
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
#include <time.h>
#include <math.h>
#include <sys/mman.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <assert.h>
#include <ctype.h>

#include <sndfile.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>
#include <pipewire/global.h>

#define DEFAULT_MEDIA_TYPE	"Audio"
#define DEFAULT_MEDIA_CATEGORY_PLAYBACK	"Playback"
#define DEFAULT_MEDIA_CATEGORY_RECORD	"Capture"
#define DEFAULT_MEDIA_ROLE	"Music"
#define DEFAULT_TARGET		"auto"
#define DEFAULT_LATENCY		"100ms"
#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2
#define DEFAULT_FORMAT		"s16"
#define DEFAULT_VOLUME		1.0

enum mode {
	mode_none,
	mode_playback,
	mode_record
};

enum unit {
	unit_none,
	unit_samples,
	unit_sec,
	unit_msec,
	unit_usec,
	unit_nsec,
};

struct data;

typedef int (*fill_fn)(struct data *d, void *dest, unsigned int n_frames);

struct target {
	struct spa_list link;
	uint32_t id;
	char *name;
	char *desc;
	int prio;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	enum mode mode;
	bool verbose;
	const char *remote_name;
	const char *media_type;
	const char *media_category;
	const char *media_role;
	const char *target;
	const char *latency;

	const char *filename;
	SNDFILE *file;
	SF_INFO info;

	unsigned int rate;
	unsigned int channels;
	unsigned int samplesize;
	unsigned int stride;
	enum unit latency_unit;
	unsigned int latency_value;

	enum spa_audio_format spa_format;

	float volume;
	bool volume_is_set;

	fill_fn fill;

	uint32_t target_id;
	bool list_targets;
	bool targets_listed;
	struct spa_list targets;

	bool drained;
};

static inline int
sf_str_to_fmt(const char *str)
{
	if (!str)
		return -1;

	if (!strcmp(str, "s8"))
		return SF_FORMAT_PCM_S8 | SF_FORMAT_WAV;
	if (!strcmp(str, "s16"))
		return SF_FORMAT_PCM_16 | SF_FORMAT_WAV;
	if (!strcmp(str, "s24"))
		return SF_FORMAT_PCM_24 | SF_FORMAT_WAV;
	if (!strcmp(str, "s32"))
		return SF_FORMAT_PCM_32 | SF_FORMAT_WAV;
	if (!strcmp(str, "f32"))
		return SF_FORMAT_FLOAT | SF_FORMAT_WAV;
	if (!strcmp(str, "f64"))
		return SF_FORMAT_DOUBLE | SF_FORMAT_WAV;

	return -1;
}

static inline const char *
sf_fmt_to_str(int format)
{
	int sub_type = (format & SF_FORMAT_SUBMASK);

	if (sub_type == SF_FORMAT_PCM_S8)
		return "s8";
	if (sub_type == SF_FORMAT_PCM_16)
		return "s16";
	if (sub_type == SF_FORMAT_PCM_24)
		return "s24";
	if (sub_type == SF_FORMAT_PCM_32)
		return "s32";
	if (sub_type == SF_FORMAT_FLOAT)
		return "f32";
	if (sub_type == SF_FORMAT_DOUBLE)
		return "f64";
	return "(invalid)";
}

#define STR_FMTS "(s8|s16|s32|f32|f64)"

/* 0 = native, 1 = le, 2 = be */
static inline int
sf_format_endianess(int format)
{
	return 0;		/* native */
}

static inline enum spa_audio_format
sf_format_to_pw(int format)
{
	int endianess;

	endianess = sf_format_endianess(format);
	if (endianess < 0)
		return SPA_AUDIO_FORMAT_UNKNOWN;

	switch (format & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_S8:
		return SPA_AUDIO_FORMAT_S8;
	case SF_FORMAT_PCM_16:
		return endianess == 1 ? SPA_AUDIO_FORMAT_S16_LE :
		       endianess == 2 ? SPA_AUDIO_FORMAT_S16_BE :
		                        SPA_AUDIO_FORMAT_S16;
	case SF_FORMAT_PCM_24:
	case SF_FORMAT_PCM_32:
		return endianess == 1 ? SPA_AUDIO_FORMAT_S32_LE :
		       endianess == 2 ? SPA_AUDIO_FORMAT_S32_BE :
		                        SPA_AUDIO_FORMAT_S32;
	case SF_FORMAT_DOUBLE:
		return endianess == 1 ? SPA_AUDIO_FORMAT_F64_LE :
		       endianess == 2 ? SPA_AUDIO_FORMAT_F64_BE :
		                        SPA_AUDIO_FORMAT_F64;
	case SF_FORMAT_FLOAT:
	default:
		return endianess == 1 ? SPA_AUDIO_FORMAT_F32_LE :
		       endianess == 2 ? SPA_AUDIO_FORMAT_F32_BE :
		                        SPA_AUDIO_FORMAT_F32;
		break;
	}

	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static inline int
sf_format_samplesize(int format)
{
	int sub_type = (format & SF_FORMAT_SUBMASK);

	switch (sub_type) {
	case SF_FORMAT_PCM_S8:
		return 1;
	case SF_FORMAT_PCM_16:
		return 2;
	case SF_FORMAT_PCM_32:
		return 4;
	case SF_FORMAT_DOUBLE:
		return 8;
	case SF_FORMAT_FLOAT:
	default:
		return 4;
	}
	return -1;
}

static int sf_playback_fill_s8(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	rn = sf_read_raw(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_s16(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(short) == sizeof(int16_t));
	rn = sf_readf_short(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_s32(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(int) == sizeof(int32_t));
	rn = sf_readf_int(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_f32(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(float) == 4);
	rn = sf_readf_float(d->file, dest, n_frames);
	return (int)rn;
}

static int sf_playback_fill_f64(struct data *d, void *dest, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(double) == 8);
	rn = sf_readf_double(d->file, dest, n_frames);
	return (int)rn;
}

static inline fill_fn
sf_fmt_playback_fill_fn(int format)
{
	enum spa_audio_format fmt = sf_format_to_pw(format);

	switch (fmt) {
	case SPA_AUDIO_FORMAT_S8:
		return sf_playback_fill_s8;
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
		/* sndfile check */
		if (sizeof(int16_t) != sizeof(short))
			return NULL;
		return sf_playback_fill_s16;
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
		/* sndfile check */
		if (sizeof(int32_t) != sizeof(int))
			return NULL;
		return sf_playback_fill_s32;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
		/* sndfile check */
		if (sizeof(float) != 4)
			return NULL;
		return sf_playback_fill_f32;
	case SPA_AUDIO_FORMAT_F64_LE:
	case SPA_AUDIO_FORMAT_F64_BE:
		if (sizeof(double) != 8)
			return NULL;
		return sf_playback_fill_f64;
	default:
		break;
	}
	return NULL;
}

static int sf_record_fill_s8(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	rn = sf_write_raw(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_s16(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(short) == sizeof(int16_t));
	rn = sf_writef_short(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_s32(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(int) == sizeof(int32_t));
	rn = sf_writef_int(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_f32(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(float) == 4);
	rn = sf_writef_float(d->file, src, n_frames);
	return (int)rn;
}

static int sf_record_fill_f64(struct data *d, void *src, unsigned int n_frames)
{
	sf_count_t rn;

	assert(sizeof(double) == 8);
	rn = sf_writef_double(d->file, src, n_frames);
	return (int)rn;
}

static inline fill_fn
sf_fmt_record_fill_fn(int format)
{
	enum spa_audio_format fmt = sf_format_to_pw(format);

	switch (fmt) {
	case SPA_AUDIO_FORMAT_S8:
		return sf_record_fill_s8;
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
		/* sndfile check */
		if (sizeof(int16_t) != sizeof(short))
			return NULL;
		return sf_record_fill_s16;
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
		/* sndfile check */
		if (sizeof(int32_t) != sizeof(int))
			return NULL;
		return sf_record_fill_s32;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
		/* sndfile check */
		if (sizeof(float) != 4)
			return NULL;
		return sf_record_fill_f32;
	case SPA_AUDIO_FORMAT_F64_LE:
	case SPA_AUDIO_FORMAT_F64_BE:
		/* sndfile check */
		if (sizeof(double) != 8)
			return NULL;
		return sf_record_fill_f64;
	default:
		break;
	}
	return NULL;
}

static void
target_destroy(struct target *target)
{
	if (!target)
		return;
	if (target->name)
		free(target->name);
	if (target->desc)
		free(target->desc);
	free(target);
}

static struct target *
target_create(uint32_t id, const char *name, const char *desc, int prio)
{
	struct target *target;

	target = malloc(sizeof(*target));
	if (!target)
		return NULL;
	target->id = id;
	target->name = strdup(name);
	target->desc = strdup(desc ? : "");
	target->prio = prio;

	if (!target->name || !target->desc) {
		target_destroy(target);
		return NULL;
	}
	return target;
}

static void on_core_info(void *userdata, const struct pw_core_info *info)
{
	struct data *data = userdata;

	if (data->verbose)
		fprintf(stdout, "remote %"PRIu32" is named \"%s\"\n",
				info->id, info->name);
}

static void on_core_done(void *userdata, uint32_t id, int seq)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("core done\n");

	/* if we're listing targets just exist */
	if (data->list_targets) {
		data->targets_listed = true;
		pw_main_loop_quit(data->loop);
	}
}

static void on_core_error(void *userdata, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = userdata;

	fprintf(stderr, "remote error: id=%"PRIu32" seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
	.error = on_core_error,
};

static void registry_event_global(void *userdata, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct data *data = userdata;
	const struct spa_dict_item *item;
	const char *name, *desc, *media_class, *prio_session;
	int prio;
	enum mode mode = mode_none;
	struct target *target;

	/* only once */
	if (data->targets_listed)
		return;

	/* must be listing targets and interface must be a node */
	if (!data->list_targets || strcmp(type, PW_TYPE_INTERFACE_Node))
		return;

	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	prio_session = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);

	/* name and media class must exist */
	if (!name || !media_class)
		return;

	/* get allowed mode from the media class */
	/* TODO extend to something else besides Audio/Source|Sink */
	if (!strcmp(media_class, "Audio/Source"))
		mode = mode_record;
	else if (!strcmp(media_class, "Audio/Sink"))
		mode = mode_playback;

	/* modes must match */
	if (mode != data->mode)
		return;

	prio = prio_session ? atoi(prio_session) : -1;

	if (data->verbose) {
		printf("registry: id=%"PRIu32" type=%s name=\"%s\" media_class=\"%s\" desc=\"%s\" prio=%d\n",
				id, type, name, media_class, desc ? : "", prio);

		spa_dict_for_each(item, props) {
			fprintf(stdout, "\t\t%s = \"%s\"\n", item->key, item->value);
		}
	}

	target = target_create(id, name, desc, prio);
	if (target)
		spa_list_append(&data->targets, &target->link);
}

static void registry_event_global_remove(void *userdata, uint32_t id)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("registry: remove id=%"PRIu32"\n", id);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void
on_state_changed(void *userdata, enum pw_stream_state old,
		 enum pw_stream_state state, const char *error)
{
	struct data *data = userdata;
	int ret;

	if (data->verbose)
		printf("stream state changed %s -> %s\n",
				pw_stream_state_as_string(old),
				pw_stream_state_as_string(state));

	if (state == PW_STREAM_STATE_STREAMING && !data->volume_is_set) {

		ret = pw_stream_set_control(data->stream,
				SPA_PROP_volume, 1, &data->volume,
				0);
		if (data->verbose)
			printf("set stream volume to %.3f - %s\n", data->volume,
					ret == 0 ? "success" : "FAILED");

		data->volume_is_set = true;

	}

	if (state == PW_STREAM_STATE_STREAMING) {
		if (data->verbose)
			printf("stream node %"PRIu32"\n",
				pw_stream_get_node_id(data->stream));
	}
}

static void
on_param_changed(void *userdata, uint32_t id, const struct spa_pod *format)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("stream param change: id=%"PRIu32"\n",
				id);
}

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	struct spa_data *d;
	int n_frames, n_fill_frames;
	uint8_t *p;
	bool have_data;
	uint32_t offset, size;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL)
		return;

	buf = b->buffer;
	d = &buf->datas[0];

	have_data = false;

	if ((p = d->data) == NULL)
		return;

	if (data->mode == mode_playback) {

		n_frames = d->maxsize / data->stride;

		n_fill_frames = data->fill(data, p, n_frames);

		if (n_fill_frames > 0) {
			d->chunk->offset = 0;
			d->chunk->stride = data->stride;
			d->chunk->size = n_fill_frames * data->stride;
			have_data = true;
		} else if (n_fill_frames < 0)
			fprintf(stderr, "fill error %d\n", n_fill_frames);
	} else {
		offset = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offset);

		p += offset;

		n_frames = size / data->stride;

		n_fill_frames = data->fill(data, p, n_frames);

		have_data = true;
	}

	if (have_data) {
		pw_stream_queue_buffer(data->stream, b);
		return;
	}

	if (data->mode == mode_playback)
		pw_stream_flush(data->stream, true);
}

static void on_drained(void *userdata)
{
	struct data *data = userdata;

	if (data->verbose)
		printf("stream drained\n");

	data->drained = true;
	pw_main_loop_quit(data->loop);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
	.drained = on_drained
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

enum {
	OPT_VERSION = 1000,
	OPT_MEDIA_TYPE,
	OPT_MEDIA_CATEGORY,
	OPT_MEDIA_ROLE,
	OPT_TARGET,
	OPT_LATENCY,
	OPT_RATE,
	OPT_CHANNELS,
	OPT_FORMAT,
	OPT_VOLUME,
	OPT_LIST_TARGETS,
};

static const struct option long_options[] = {
	{"help",	no_argument,	   NULL, 'h'},
	{"version",	no_argument,	   NULL, OPT_VERSION},
	{"verbose",	no_argument,	   NULL, 'v'},

	{"record",	no_argument,	   NULL, 'r'},
	{"playback",	no_argument,	   NULL, 's'},

	{"remote",	required_argument, NULL, 'R'},

	{"media-type",		required_argument, NULL, OPT_MEDIA_TYPE },
	{"media-category",	required_argument, NULL, OPT_MEDIA_CATEGORY },
	{"media-role",		required_argument, NULL, OPT_MEDIA_ROLE },
	{"target",		required_argument, NULL, OPT_TARGET },
	{"latency",		required_argument, NULL, OPT_LATENCY },

	{"rate",		required_argument, NULL, OPT_RATE },
	{"channels",		required_argument, NULL, OPT_CHANNELS },
	{"format",		required_argument, NULL, OPT_FORMAT },

	{"volume",		required_argument, NULL, OPT_VOLUME },
	{"list-targets",	no_argument, NULL, OPT_LIST_TARGETS },

	{NULL, 0, NULL, 0 }
};

static void show_usage(const char *name, bool is_error)
{
	FILE *fp;

	fp = is_error ? stderr : stdout;

        fprintf(fp, "%s [options] <file>\n", name);

	fprintf(fp,
             "  -h, --help                            Show this help\n"
             "      --version                         Show version\n"
	     "\n");

	fprintf(fp,
             "  -r, --remote                          Remote daemon name\n"
             "      --media-type                      Set media type (default %s)\n"
             "      --media-category                  Set media category (default %s)\n"
             "      --media-role                      Set media role (default %s)\n"
             "      --target                          Set node target (default %s)\n"
             "      --latency                         Set node latency (default %s)\n"
	     "                                          Xunit (unit = s, ms, us, ns)\n"
	     "                                          or direct samples (256)\n"
	     "                                          the rate is the one of the source file\n"
	     "      --list-targets                    List available targets for --target\n"
	     "\n",
	     DEFAULT_MEDIA_TYPE,
	     DEFAULT_MEDIA_CATEGORY_PLAYBACK,
	     DEFAULT_MEDIA_ROLE,
	     DEFAULT_TARGET, DEFAULT_LATENCY);

	fprintf(fp,
             "      --rate                            Sample rate (req. for rec) (default %u)\n"
             "      --channels                        Number of channels (req. for rec) (default %u)\n"
             "      --format                          Sample format %s (req. for rec) (default %s)\n"
	     "      --volume                          Stream volume 0-1.0 (default %.3f)\n"
	     "\n",
	     DEFAULT_RATE, DEFAULT_CHANNELS, STR_FMTS, DEFAULT_FORMAT, DEFAULT_VOLUME);

	if (!strcmp(name, "pwcat")) {
		fprintf(fp,
		     "  -p, --playback                        Playback mode\n"
		     "  -r, --record                          Recording mode\n"
		     "\n");
	}

        fprintf(fp,
             "  -v, --verbose                         Enable verbose operations\n"
	     "\n");
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	struct pw_loop *l;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const char *prog;
	int exit_code = EXIT_FAILURE, c, format = 0, ret;
	struct pw_properties *props = NULL;
	const char *s;
	unsigned int nom = 0;
	struct target *target, *target_default;

	pw_init(&argc, &argv);

	prog = argv[0];
	if ((prog = strrchr(argv[0], '/')) != NULL)
		prog++;
	else
		prog = argv[0];

	/* prime the mode from the program name */
	if (!strcmp(prog, "pw-play"))
		data.mode = mode_playback;
	else if (!strcmp(prog, "pw-record"))
		data.mode = mode_record;
	else
		data.mode = mode_none;

	/* negative means no volume adjustment */
	data.volume = -1.0;

	/* initialize list everytime */
	spa_list_init(&data.targets);

	while ((c = getopt_long(argc, argv, "hvprR:", long_options, NULL)) != -1) {

		switch (c) {

		case 'h':
			show_usage(prog, false);
			return EXIT_SUCCESS;

		case OPT_VERSION:
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				prog,
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;

		case 'v':
			data.verbose = true;
			break;

		case 'p':
			data.mode = mode_playback;
			break;

		case 'r':
			data.mode = mode_record;
			break;

		case 'R':
			data.remote_name = optarg;
			break;

		case OPT_MEDIA_TYPE:
			data.media_type = optarg;
			break;

		case OPT_MEDIA_CATEGORY:
			data.media_category = optarg;
			break;

		case OPT_MEDIA_ROLE:
			data.media_role = optarg;
			break;

		case OPT_TARGET:
			data.target = optarg;
			if (!strcmp(optarg, "auto")) {
				data.target_id = PW_ID_ANY;
				break;
			}
			if (!isdigit(optarg[0])) {
				fprintf(stderr, "error: bad target option \"%s\"\n", optarg);
				goto error_usage;
			}
			data.target_id = atoi(optarg);
			break;

		case OPT_LATENCY:
			data.latency = optarg;
			break;

		case OPT_RATE:
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad rate %d\n", ret);
				goto error_usage;
			}
			data.rate = (unsigned int)ret;
			break;

		case OPT_CHANNELS:
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad channels %d\n", ret);
				goto error_usage;
			}
			data.channels = (unsigned int)ret;
			break;

		case OPT_FORMAT:
			if (sf_str_to_fmt(optarg) == -1) {
				fprintf(stderr, "error: unknown format \"%s\"\n", optarg);
				goto error_usage;
			}
			format = sf_str_to_fmt(optarg);
			break;

		case OPT_VOLUME:
			data.volume = atof(optarg);
			break;

		case OPT_LIST_TARGETS:
			data.list_targets = true;
			break;

		default:
			fprintf(stderr, "error: unknown option '%c'\n", c);
			goto error_usage;
		}
	}

	if (data.mode == mode_none) {
		fprintf(stderr, "error: one of the playback/record options must be provided\n");
		goto error_usage;
	}

	if (!data.media_type)
		data.media_type = DEFAULT_MEDIA_TYPE;
	if (!data.media_category)
		data.media_category = data.mode == mode_playback ?
					DEFAULT_MEDIA_CATEGORY_PLAYBACK :
					DEFAULT_MEDIA_CATEGORY_RECORD;
	if (!data.media_role)
		data.media_role = DEFAULT_MEDIA_ROLE;
	if (!data.target) {
		data.target = DEFAULT_TARGET;
		data.target_id = PW_ID_ANY;
	}
	if (!data.latency)
		data.latency = DEFAULT_LATENCY;
	if (!data.rate)
		data.rate = DEFAULT_RATE;
	if (!data.channels)
		data.rate = DEFAULT_CHANNELS;
	if (data.volume < 0)
		data.volume = DEFAULT_VOLUME;
	if (data.mode == mode_record && !format)
		format = sf_str_to_fmt(DEFAULT_FORMAT);

	if (!data.list_targets && optind >= argc) {
		fprintf(stderr, "error: filename argument missing\n");
		goto error_usage;
	}
	data.filename = argv[optind++];

	/* for record, you fill in the info first */
	if (data.mode == mode_record) {
		memset(&data.info, 0, sizeof(data.info));
		data.info.samplerate = data.rate;
		data.info.channels = data.channels;
		data.info.format = format;
	}

	if (!data.list_targets) {
		data.file = sf_open(data.filename,
				data.mode == mode_playback ? SFM_READ : SFM_WRITE,
				&data.info);
		if (!data.file) {
			fprintf(stderr, "error: failed to open audio file \"%s\": %s\n",
					data.filename, sf_strerror(NULL));
			goto error_open_file;
		}

		if (data.verbose)
			printf("opened file \"%s\" format %08x\n", data.filename, data.info.format);

		format = data.info.format;
		if (data.mode == mode_playback) {
			data.rate = data.info.samplerate;
			data.channels = data.info.channels;
		}
		data.samplesize = sf_format_samplesize(format);
		data.stride = data.samplesize * data.channels;
		data.spa_format = sf_format_to_pw(format);
		data.fill = data.mode == mode_playback ?
				sf_fmt_playback_fill_fn(format) :
				sf_fmt_record_fill_fn(format);

		data.latency_unit = unit_none;
		s = data.latency;
		while (*s && isdigit(*s))
			s++;
		if (!*s)
			data.latency_unit = unit_samples;
		else if (!strcmp(s, "none"))
			data.latency_unit = unit_none;
		else if (!strcmp(s, "s") || !strcmp(s, "sec") || !strcmp(s, "secs"))
			data.latency_unit = unit_sec;
		else if (!strcmp(s, "ms") || !strcmp(s, "msec") || !strcmp(s, "msecs"))
			data.latency_unit = unit_msec;
		else if (!strcmp(s, "us") || !strcmp(s, "usec") || !strcmp(s, "usecs"))
			data.latency_unit = unit_usec;
		else if (!strcmp(s, "ns") || !strcmp(s, "nsec") || !strcmp(s, "nsecs"))
			data.latency_unit = unit_nsec;
		else {
			fprintf(stderr, "error: bad latency value %s (bad unit)\n", data.latency);
			goto error_bad_file;
		}
		data.latency_value = atoi(data.latency);
		if (!data.latency_value) {
			fprintf(stderr, "error: bad latency value %s (is zero)\n", data.latency);
			goto error_bad_file;
		}

		switch (data.latency_unit) {
		case unit_sec:
			nom = data.latency_value * data.rate;
			break;
		case unit_msec:
			nom = nearbyint((data.latency_value * data.rate) / 1000.0);
			break;
		case unit_usec:
			nom = nearbyint((data.latency_value * data.rate) / 1000000.0);
			break;
		case unit_nsec:
			nom = nearbyint((data.latency_value * data.rate) / 1000000000.0);
			break;
		case unit_samples:
			nom = data.latency_value;
			break;
		default:
			nom = 0;
			break;
		}

		if (data.verbose)
			printf("rate=%u channels=%u fmt=%s samplesize=%u stride=%u latency=%u (%.3fs)\n",
					data.rate, data.channels,
					sf_fmt_to_str(format),
					data.samplesize,
					data.stride, nom, (double)nom/data.rate);
	}

	/* make a main loop. If you already have another main loop, you can add
	 * the fd of this pipewire mainloop to it. */
	data.loop = pw_main_loop_new(NULL);
	if (!data.loop) {
		fprintf(stderr, "error: pw_main_loop_new() failed\n");
		goto error_no_main_loop;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (!data.context) {
		fprintf(stderr, "error: pw_context_new() failed\n");
		goto error_no_context;
	}

	props = pw_properties_new(
			PW_KEY_MEDIA_TYPE, data.media_type,
			PW_KEY_MEDIA_CATEGORY, data.media_category,
			PW_KEY_MEDIA_ROLE, data.media_role,
			PW_KEY_APP_NAME, prog,
			PW_KEY_MEDIA_NAME, data.filename,
			PW_KEY_NODE_NAME, prog,
			NULL);
	if (!props) {
		fprintf(stderr, "error: pw_properties_new() failed\n");
		goto error_no_props;
	}

	if (nom)
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", nom, data.rate);

	data.core = pw_context_connect(data.context, NULL, 0);
	if (!data.core) {
		fprintf(stderr, "error: pw_context_connect() failed\n");
		goto error_ctx_connect_failed;
	}
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);

	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	if (!data.registry) {
		fprintf(stderr, "error: pw_core_get_registry() failed\n");
		goto error_no_registry;
	}
	pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);

	pw_core_sync(data.core, 0, 0);

	if (!data.list_targets) {
		data.stream = pw_stream_new(data.core, prog, props);
		if (!data.stream) {
			fprintf(stderr, "error: failed to create simple stream\n");
			goto error_no_stream;
		}
		props = NULL;
		pw_stream_add_listener(data.stream, &data.stream_listener, &stream_events, &data);

		params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
				&SPA_AUDIO_INFO_RAW_INIT(
					.format = data.spa_format,
					.channels = data.channels,
					.rate = data.rate ));

		if (data.verbose)
			printf("connecting %s stream; target_id=%"PRIu32"\n",
					data.mode == mode_playback ? "playback" : "record",
					data.target_id);

		ret = pw_stream_connect(data.stream,
				  data.mode == mode_playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
				  data.target_id,
				  PW_STREAM_FLAG_AUTOCONNECT |
				  PW_STREAM_FLAG_MAP_BUFFERS |
				  PW_STREAM_FLAG_RT_PROCESS,
				  params, 1);
		if (ret != 0) {
			fprintf(stderr, "error: failed connect\n");
			goto error_connect_fail;
		}

		if (data.verbose) {
			const struct pw_properties *props;
			void *pstate;
			const char *key, *val;

			if ((props = pw_stream_get_properties(data.stream)) != NULL) {
				printf("stream properties:\n");
				pstate = NULL;
				while ((key = pw_properties_iterate(props, &pstate)) != NULL &&
					(val = pw_properties_get(props, key)) != NULL) {
					printf("\t%s = \"%s\"\n", key, val);
				}
			}
		}
	}

	/* and wait while we let things run */
	pw_main_loop_run(data.loop);

	/* we're returning OK only if got to the point to drain */
	if (!data.list_targets) {
		if (data.drained)
			exit_code = EXIT_SUCCESS;
	} else {
		if (data.targets_listed) {
			exit_code = EXIT_SUCCESS;

			/* first find the highest priority */
			target_default = NULL;
			spa_list_for_each(target, &data.targets, link) {
				if (!target_default) {
					target_default = target;
					continue;
				}
				if (target->prio > target_default->prio)
					target_default = target;
			}

			printf("Available targets (\"*\" denotes default):\n");
			spa_list_for_each(target, &data.targets, link) {
				printf("%s\t%"PRIu32": name=\"%s\" description=\"%s\" prio=%d\n",
				       target == target_default ? "*" : "",
				       target->id, target->name, target->desc, target->prio);
			}
		}
	}

	/* destroy targets */
	while (!spa_list_is_empty(&data.targets)) {
		target = spa_list_last(&data.targets, struct target, link);
		spa_list_remove(&target->link);
		target_destroy(target);
	}

error_connect_fail:
	if (data.stream)
		pw_stream_destroy(data.stream);
error_no_stream:
error_no_registry:
	pw_core_disconnect(data.core);
error_ctx_connect_failed:
	if (props)
		pw_properties_free(props);
error_no_props:
	pw_context_destroy(data.context);

error_no_context:
	pw_main_loop_destroy(data.loop);
error_no_main_loop:
error_bad_file:

	if (data.file)
		sf_close(data.file);
error_open_file:

	return exit_code;

error_usage:
	show_usage(prog, true);
	return EXIT_FAILURE;
}
