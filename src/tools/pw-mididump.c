/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <getopt.h>
#include <locale.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>
#include <spa/control/control.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include "midifile.h"
#include "midiclip.h"

struct data;

struct port {
	struct data *data;
};

struct data {
	struct pw_main_loop *loop;
	const char *opt_remote;
	struct pw_filter *filter;
	struct port *in_port;
	int64_t clock_time;
	bool opt_midi1;
};


static int dump_clip(const char *filename)
{
	struct midi_clip *file;
	struct midi_clip_info info;
	struct midi_event ev;

	file = midi_clip_open(filename, "r", &info);
	if (file == NULL) {
		fprintf(stderr, "error opening %s: %m\n", filename);
		return -1;
	}

	printf("opened %s format:%u division:%u\n", filename, info.format, info.division);

	while (midi_clip_read_event(file, &ev) == 1)
		midi_event_dump(stdout, &ev);

	midi_clip_close(file);

	return 0;
}

static int dump_file(const char *filename)
{
	struct midi_file *file;
	struct midi_file_info info;
	struct midi_event ev;

	file = midi_file_open(filename, "r", &info);
	if (file == NULL) {
		return dump_clip(filename);
	}

	printf("opened %s format:%u ntracks:%u division:%u\n", filename, info.format, info.ntracks, info.division);

	while (midi_file_read_event(file, &ev) == 1)
		midi_event_dump(stdout, &ev);

	midi_file_close(file);

	return 0;
}

static void on_process(void *_data, struct spa_io_position *position)
{
	struct data *data = _data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	struct spa_data *d;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct spa_pod_sequence seq;
	const void *seq_body, *c_body;
	struct spa_pod_control c;
	uint64_t offset;

	offset = data->clock_time;
	data->clock_time += position->clock.duration;

	b = pw_filter_dequeue_buffer(data->in_port);
	if (b == NULL)
		return;

	buf = b->buffer;
	d = &buf->datas[0];

	if (d->data == NULL)
		goto done;

	spa_pod_parser_init_from_data(&parser, d->data, d->maxsize, d->chunk->offset, d->chunk->size);

	if (spa_pod_parser_push_sequence_body(&parser, &frame, &seq, &seq_body) < 0)
		goto done;

	while (spa_pod_parser_get_control_body(&parser, &c, &c_body) >= 0) {
		struct midi_event ev;

		switch (c.type) {
		case SPA_CONTROL_UMP:
			ev.type = MIDI_EVENT_TYPE_UMP;
			break;
		case SPA_CONTROL_Midi:
			ev.type = MIDI_EVENT_TYPE_MIDI1;
			break;
		default:
			continue;
		}
		ev.track = 0;
		ev.sec = (offset + c.offset) / (float) position->clock.rate.denom;
		ev.data = (uint8_t*)c_body;
		ev.size = c.value.size;

		fprintf(stdout, "%4d: ", c.offset);
		midi_event_dump(stdout, &ev);
	}

done:
	pw_filter_queue_buffer(data->in_port, b);
}

static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

static int dump_filter(struct data *data)
{
	data->loop = pw_main_loop_new(NULL);
	if (data->loop == NULL)
		return -errno;

	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGINT, do_quit, data);
	pw_loop_add_signal(pw_main_loop_get_loop(data->loop), SIGTERM, do_quit, data);

	data->filter = pw_filter_new_simple(
			pw_main_loop_get_loop(data->loop),
			"midi-dump",
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data->opt_remote,
				PW_KEY_MEDIA_TYPE, "Midi",
				PW_KEY_MEDIA_CATEGORY, "Filter",
				PW_KEY_MEDIA_ROLE, "DSP",
				NULL),
			&filter_events,
			data);

	data->in_port = pw_filter_add_port(data->filter,
			PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, data->opt_midi1 ? "8 bit raw midi" : "32 bit raw UMP",
				PW_KEY_PORT_NAME, "input",
				NULL),
			NULL, 0);

	if (pw_filter_connect(data->filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
		fprintf(stderr, "can't connect\n");
		return -1;
	}

	pw_main_loop_run(data->loop);

	pw_filter_destroy(data->filter);
	pw_main_loop_destroy(data->loop);

	return 0;
}

static void show_help(const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "%s [options] [FILE]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
	        "  -M, --force-midi                      Force midi format, one of \"midi\" or \"ump\",(default ump)\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	int res = 0, c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "force-midi",	required_argument,	NULL, 'M' },
		{ NULL,	0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	setlinebuf(stdout);

	while ((c = getopt_long(argc, argv, "hVr:M:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0], false);
			return 0;
		case 'V':
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			data.opt_remote = optarg;
			break;

		case 'M':
			if (spa_streq(optarg, "midi"))
				data.opt_midi1 = true;
			else if (spa_streq(optarg, "ump"))
				data.opt_midi1 = false;
			else {
				fprintf(stderr, "error: bad force-midi %s\n", optarg);
				show_help(argv[0], true);
				return 0;
			}
			break;

		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	if (optind < argc) {
		res = dump_file(argv[optind]);
	} else {
		res = dump_filter(&data);
	}
	pw_deinit();
	return res;
}
