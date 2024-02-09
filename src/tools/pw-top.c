/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <locale.h>
#include <ncurses.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/pod/parser.h>
#include <spa/debug/types.h>
#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>

#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>

#define MAX_FORMAT		16
#define MAX_NAME		128

#define XRUN_INVALID	(uint32_t)-1

struct driver {
	int64_t count;
	float cpu_load[3];
	struct spa_io_clock clock;
	uint32_t xrun_count;
};

struct measurement {
	int32_t index;
	int32_t status;
	int64_t quantum;
	int64_t prev_signal;
	int64_t signal;
	int64_t awake;
	int64_t finish;
	struct spa_fraction latency;
	uint32_t xrun_count;
};

struct node {
	struct spa_list link;
	struct data *data;
	uint32_t id;
	char name[MAX_NAME+1];
	enum pw_node_state state;
	struct measurement measurement;
	struct driver info;
	struct node *driver;
	uint32_t generation;
	char format[MAX_FORMAT+1];
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	unsigned int inactive:1;
	struct spa_hook object_listener;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_proxy *profiler;
	struct spa_hook profiler_listener;
	int check_profiler;

	struct spa_source *timer;

	int n_nodes;
	struct spa_list node_list;
	uint32_t generation;
	unsigned pending_refresh:1;

	WINDOW *win;

	unsigned int batch_mode:1;
	int iterations;
};

struct point {
	struct node *driver;
	struct driver info;
};

static SPA_PRINTF_FUNC(4, 5) void print_mode_dependent(struct data *d, int y, int x, const char *fmt, ...)
{
	va_list argp;
	if (!d->batch_mode)
		mvwprintw(d->win, y, x, "%s", "");

	va_start(argp, fmt);
	if (d->batch_mode) {
		vprintf(fmt, argp);
		printf("\n");
	} else
		vw_printw(d->win, fmt, argp);
	va_end(argp);
}


static int process_info(struct data *d, const struct spa_pod *pod, struct driver *info)
{
	return spa_pod_parse_struct(pod,
			SPA_POD_Long(&info->count),
			SPA_POD_Float(&info->cpu_load[0]),
			SPA_POD_Float(&info->cpu_load[1]),
			SPA_POD_Float(&info->cpu_load[2]),
			SPA_POD_Int(&info->xrun_count));
}

static int process_clock(struct data *d, const struct spa_pod *pod, struct driver *info)
{
	return spa_pod_parse_struct(pod,
			SPA_POD_Int(&info->clock.flags),
			SPA_POD_Int(&info->clock.id),
			SPA_POD_Stringn(info->clock.name, sizeof(info->clock.name)),
			SPA_POD_Long(&info->clock.nsec),
			SPA_POD_Fraction(&info->clock.rate),
			SPA_POD_Long(&info->clock.position),
			SPA_POD_Long(&info->clock.duration),
			SPA_POD_Long(&info->clock.delay),
			SPA_POD_Double(&info->clock.rate_diff),
			SPA_POD_Long(&info->clock.next_nsec));
}

static struct node *find_node(struct data *d, uint32_t id)
{
	struct node *n;
	spa_list_for_each(n, &d->node_list, link) {
		if (n->id == id)
			return n;
	}
	return NULL;
}

static void on_node_removed(void *data)
{
	struct node *n = data;
	pw_proxy_destroy(n->proxy);
}

static void on_node_destroy(void *data)
{
	struct node *n = data;
	n->proxy = NULL;
	spa_hook_remove(&n->proxy_listener);
	spa_hook_remove(&n->object_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_node_removed,
	.destroy = on_node_destroy,
};

static void do_refresh(struct data *d, bool force_refresh);

static const char *find_node_name(const struct spa_dict *props)
{
	static const char * const name_keys[] = {
		PW_KEY_NODE_NAME,
		PW_KEY_NODE_DESCRIPTION,
		PW_KEY_APP_NAME,
		PW_KEY_MEDIA_NAME,
	};

	SPA_FOR_EACH_ELEMENT_VAR(name_keys, key) {
		const char *name = spa_dict_lookup(props, *key);
		if (name)
			return name;
	}

	return NULL;
}

static void set_node_name(struct node *n, const char *name)
{
	if (name)
		snprintf(n->name, sizeof(n->name), "%s", name);
	else
		snprintf(n->name, sizeof(n->name), "%u", n->id);
}

static void node_info(void *data, const struct pw_node_info *info)
{
	struct node *n = data;

	if (n->state != info->state) {
		n->state = info->state;
		do_refresh(n->data, !n->data->batch_mode);
	}

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
		set_node_name(n, find_node_name(info->props));
}

static void node_param(void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			const struct spa_pod *param)
{
	struct node *n = data;

	if (param == NULL) {
		spa_zero(n->format);
		goto done;
	}

	switch (id) {
	case SPA_PARAM_Format:
	{
		uint32_t media_type, media_subtype;

		if (spa_format_parse(param, &media_type, &media_subtype) < 0)
			goto done;

		switch(media_type) {
		case SPA_MEDIA_TYPE_audio:
			switch(media_subtype) {
			case SPA_MEDIA_SUBTYPE_raw:
			{
				struct spa_audio_info_raw info = { 0 };
				if (spa_format_audio_raw_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "%6.6s %d %d",
						spa_debug_type_find_short_name(
							spa_type_audio_format, info.format),
						info.channels, info.rate);
				}
				break;
			}
			case SPA_MEDIA_SUBTYPE_dsd:
			{
				struct spa_audio_info_dsd info = { 0 };
				if (spa_format_audio_dsd_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "DSD%d %d ",
						8 * info.rate / 44100, info.channels);

				}
				break;
			}
			case SPA_MEDIA_SUBTYPE_iec958:
			{
				struct spa_audio_info_iec958 info = { 0 };
				if (spa_format_audio_iec958_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "IEC958 %s %d",
						spa_debug_type_find_short_name(
							spa_type_audio_iec958_codec, info.codec),
						info.rate);

				}
				break;
			}
			}
			break;
		case SPA_MEDIA_TYPE_video:
			switch(media_subtype) {
			case SPA_MEDIA_SUBTYPE_raw:
			{
				struct spa_video_info_raw info = { 0 };
				if (spa_format_video_raw_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "%6.6s %dx%d",
						spa_debug_type_find_short_name(spa_type_video_format, info.format),
						info.size.width, info.size.height);
				}
				break;
			}
			case SPA_MEDIA_SUBTYPE_mjpg:
			{
				struct spa_video_info_mjpg info = { 0 };
				if (spa_format_video_mjpg_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "MJPG %dx%d",
						info.size.width, info.size.height);
				}
				break;
			}
			case SPA_MEDIA_SUBTYPE_h264:
			{
				struct spa_video_info_h264 info = { 0 };
				if (spa_format_video_h264_parse(param, &info) >= 0) {
					snprintf(n->format, sizeof(n->format), "H264 %dx%d",
						info.size.width, info.size.height);
				}
				break;
			}
			}
			break;
		case SPA_MEDIA_TYPE_application:
			switch(media_subtype) {
			case SPA_MEDIA_SUBTYPE_control:
				snprintf(n->format, sizeof(n->format), "%s", "CONTROL");
				break;
			}
			break;
		}
		break;
	}
	default:
		break;
	}
done:
	do_refresh(n->data, !n->data->batch_mode);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
	.param = node_param,
};

static struct node *add_node(struct data *d, uint32_t id, const char *name)
{
	struct node *n;

	if ((n = calloc(1, sizeof(*n))) == NULL)
		return NULL;

	n->data = d;
	n->id = id;
	n->driver = n;

	n->proxy = pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (n->proxy) {
		uint32_t ids[1] = { SPA_PARAM_Format };

		pw_proxy_add_listener(n->proxy,
				&n->proxy_listener, &proxy_events, n);
		pw_proxy_add_object_listener(n->proxy,
				&n->object_listener, &node_events, n);

		pw_node_subscribe_params((struct pw_node*)n->proxy,
						ids, 1);
	}
	spa_list_append(&d->node_list, &n->link);
	d->n_nodes++;
	if (!d->batch_mode)
		d->pending_refresh = true;

	set_node_name(n, name);

	return n;
}

static void remove_node(struct data *d, struct node *n)
{
	if (n->proxy)
		pw_proxy_destroy(n->proxy);
	spa_list_remove(&n->link);
	d->n_nodes--;
	if (!d->batch_mode)
		d->pending_refresh = true;
	free(n);
}

static int process_driver_block(struct data *d, const struct spa_pod *pod, struct point *point)
{
	char *name = NULL;
	uint32_t id = 0;
	struct measurement m;
	struct node *n;
	int res;

	spa_zero(m);
	m.xrun_count = XRUN_INVALID;
	if ((res = spa_pod_parse_struct(pod,
			SPA_POD_Int(&id),
			SPA_POD_String(&name),
			SPA_POD_Long(&m.prev_signal),
			SPA_POD_Long(&m.signal),
			SPA_POD_Long(&m.awake),
			SPA_POD_Long(&m.finish),
			SPA_POD_Int(&m.status),
			SPA_POD_Fraction(&m.latency),
			SPA_POD_OPT_Int(&m.xrun_count))) < 0)
		return res;

	if ((n = find_node(d, id)) == NULL)
		return -ENOENT;

	n->driver = n;
	n->measurement = m;
	n->info = point->info;
	point->driver = n;
	n->generation = d->generation;
	return 0;
}

static int process_follower_block(struct data *d, const struct spa_pod *pod, struct point *point)
{
	uint32_t id = 0;
	const char *name =  NULL;
	struct measurement m;
	struct node *n;
	int res;

	spa_zero(m);
	m.xrun_count = XRUN_INVALID;
	if ((res = spa_pod_parse_struct(pod,
			SPA_POD_Int(&id),
			SPA_POD_String(&name),
			SPA_POD_Long(&m.prev_signal),
			SPA_POD_Long(&m.signal),
			SPA_POD_Long(&m.awake),
			SPA_POD_Long(&m.finish),
			SPA_POD_Int(&m.status),
			SPA_POD_Fraction(&m.latency),
			SPA_POD_OPT_Int(&m.xrun_count))) < 0)
		return res;

	if ((n = find_node(d, id)) == NULL)
		return -ENOENT;

	n->measurement = m;
	if (n->driver != point->driver) {
		n->driver = point->driver;
		d->pending_refresh = true;
	}
	n->generation = d->generation;
	return 0;
}

static const char *print_time(char *buf, bool active, size_t len, uint64_t val)
{
	if (val == (uint64_t)-1 || !active)
		snprintf(buf, len, "   --- ");
	else if (val == (uint64_t)-2)
		snprintf(buf, len, "   +++ ");
	else if (val < 1000000llu)
		snprintf(buf, len, "%5.1fus", val/1000.f);
	else if (val < 1000000000llu)
		snprintf(buf, len, "%5.1fms", val/1000000.f);
	else
		snprintf(buf, len, "%5.1fs", val/1000000000.f);
	return buf;
}

static const char *print_perc(char *buf, bool active, size_t len, uint64_t val, float quantum)
{
	if (val == (uint64_t)-1 || !active) {
		snprintf(buf, len, " --- ");
	} else if (val == (uint64_t)-2) {
		snprintf(buf, len, " +++ ");
	} else {
		float frac = val / 1000000000.f;
		snprintf(buf, len, "%5.2f", quantum == 0.0f ? 0.0f : frac/quantum);
	}
	return buf;
}

static const char *state_as_string(enum pw_node_state state)
{
	switch (state) {
	case PW_NODE_STATE_ERROR:
		return "E";
	case PW_NODE_STATE_CREATING:
		return "C";
	case PW_NODE_STATE_SUSPENDED:
		return "S";
	case PW_NODE_STATE_IDLE:
		return "I";
	case PW_NODE_STATE_RUNNING:
		return "R";
	}
	return "!";
}

static void print_node(struct data *d, struct driver *i, struct node *n, int y)
{
	char buf1[64];
	char buf2[64];
	char buf3[64];
	char buf4[64];
	uint64_t waiting, busy;
	float quantum;
	struct spa_fraction frac;
	bool active;

	active = n->state == PW_NODE_STATE_RUNNING || n->state == PW_NODE_STATE_IDLE;

	if (!active)
		frac = SPA_FRACTION(0, 0);
	else if (n->driver == n)
		frac = SPA_FRACTION((uint32_t)(i->clock.duration * i->clock.rate.num), i->clock.rate.denom);
	else
		frac = SPA_FRACTION(n->measurement.latency.num, n->measurement.latency.denom);

	if (i->clock.rate.denom)
		quantum = (float)i->clock.duration * i->clock.rate.num / (float)i->clock.rate.denom;
	else
		quantum = 0.0;

	if (n->measurement.awake >= n->measurement.signal)
		waiting = n->measurement.awake - n->measurement.signal;
	else if (n->measurement.signal > n->measurement.prev_signal)
		waiting = -2;
	else
		waiting = -1;

	if (n->measurement.finish >= n->measurement.awake)
		busy = n->measurement.finish - n->measurement.awake;
	else if (n->measurement.awake > n->measurement.prev_signal)
		busy = -2;
	else
		busy = -1;

	print_mode_dependent(d, y, 0, "%s %4.1u %6.1u %6.1u %s %s %s %s  %3.1u %16.16s %s%s",
			state_as_string(n->state),
			n->id,
			frac.num, frac.denom,
			print_time(buf1, active, 64, waiting),
			print_time(buf2, active, 64, busy),
			print_perc(buf3, active, 64, waiting, quantum),
			print_perc(buf4, active, 64, busy, quantum),
			n->measurement.xrun_count == XRUN_INVALID ?
					i->xrun_count : n->measurement.xrun_count,
			active ? n->format : "",
			n->driver == n ? "" : " + ",
			n->name);
}

static void clear_node(struct node *n)
{
	n->driver = n;
	spa_zero(n->measurement);
	spa_zero(n->info);
}

#define HEADER	"S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME "

static void do_refresh(struct data *d, bool force_refresh)
{
	struct node *n, *t, *f;
	int y = 1;

	if (!d->pending_refresh && !force_refresh)
		return;

	if (!d->batch_mode) {
		wclear(d->win);
		wattron(d->win, A_REVERSE);
		wprintw(d->win, "%-*.*s", COLS, COLS, HEADER);
		wattroff(d->win, A_REVERSE);
		wprintw(d->win, "\n");
	} else
		printf(HEADER "\n");

	spa_list_for_each_safe(n, t, &d->node_list, link) {
		if (n->driver != n)
			continue;

		print_node(d, &n->info, n, y++);
		if(!d->batch_mode && y > LINES)
			break;

		spa_list_for_each(f, &d->node_list, link) {
			if (d->generation > f->generation + 22)
				clear_node(f);

			if (f->driver != n || f == n)
				continue;

			print_node(d, &n->info, f, y++);
			if(y > LINES)
				break;

		}
	}

	if (!d->batch_mode) {
		// Clear from last line to the end of the window to hide text wrapping from the last node
		wmove(d->win, y, 0);
		wclrtobot(d->win);

		wrefresh(d->win);
	}

	d->pending_refresh = false;

	if (d->iterations > 0)
		d->iterations--;

	if (d->iterations == 0)
		pw_main_loop_quit(d->loop);
}

static void do_timeout(void *data, uint64_t expirations)
{
	struct data *d = data;
	d->generation++;
	do_refresh(d, true);
}

static void profiler_profile(void *data, const struct spa_pod *pod)
{
	struct data *d = data;
	struct spa_pod *o;
	struct spa_pod_prop *p;
	struct point point;

	SPA_POD_STRUCT_FOREACH(pod, o) {
		int res = 0;
		if (!spa_pod_is_object_type(o, SPA_TYPE_OBJECT_Profiler))
			continue;

		spa_zero(point);
		SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)o, p) {
			switch(p->key) {
			case SPA_PROFILER_info:
				res = process_info(d, &p->value, &point.info);
				break;
			case SPA_PROFILER_clock:
				res = process_clock(d, &p->value, &point.info);
				break;
			case SPA_PROFILER_driverBlock:
				res = process_driver_block(d, &p->value, &point);
				break;
			case SPA_PROFILER_followerBlock:
				process_follower_block(d, &p->value, &point);
				break;
			default:
				break;
			}
			if (res < 0)
				break;
		}
		if (res < 0)
			continue;
	}

	do_refresh(d, false);
}

static const struct pw_profiler_events profiler_events = {
	PW_VERSION_PROFILER_EVENTS,
	.profile = profiler_profile,
};

static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;
	struct pw_proxy *proxy;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		if (add_node(d, id, find_node_name(props)) == NULL)
			pw_log_warn("can add node %u: %m", id);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Profiler)) {
		if (d->profiler != NULL) {
			printf("Ignoring profiler %d: already attached\n", id);
			return;
		}

		proxy = pw_registry_bind(d->registry, id, type, PW_VERSION_PROFILER, 0);
		if (proxy == NULL)
			goto error_proxy;

		d->profiler = proxy;
		pw_proxy_add_object_listener(proxy, &d->profiler_listener, &profiler_events, d);
	}

	do_refresh(d, false);
	return;

error_proxy:
	pw_log_error("failed to create proxy: %m");
	return;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct data *d = data;
	struct node *n;
	if ((n = find_node(d, id)) != NULL)
		remove_node(d, n);

	do_refresh(d, false);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	if (id == PW_ID_CORE) {
		switch (res) {
		case -EPIPE:
			pw_main_loop_quit(data->loop);
			break;
		default:
			pw_log_error("error id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
			break;
		}
	} else {
		pw_log_info("error id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	}
}

static void on_core_done(void *_data, uint32_t id, int seq)
{
	struct data *d = _data;

	if (seq == d->check_profiler) {
		if (d->profiler == NULL) {
			pw_log_error("no Profiler Interface found, please load one in the server");
			pw_main_loop_quit(d->loop);
		} else {
			do_refresh(d, true);
		}
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
	.done = on_core_done,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "Usage:\n%s [options]\n\n"
		"Options:\n"
		"  -b, --batch-mode		         run in non-interactive batch mode\n"
		"  -n, --iterations = NUMBER             exit after NUMBER batch iterations\n"
		"  -r, --remote                          Remote daemon name\n"
		"\n"
		"  -h, --help                            Show this help\n"
		"  -V  --version                         Show version\n",
		name);
}

static void terminal_start(void)
{
	initscr();
	cbreak();
	noecho();
	refresh();
}

static void terminal_stop(void)
{
	endwin();
}

static void do_handle_io(void *data, int fd, uint32_t mask)
{
	struct data *d = data;

	if (mask & SPA_IO_IN) {
		int ch = getch();

		switch(ch) {
		case 'q':
			pw_main_loop_quit(d->loop);
			break;
		default:
			do_refresh(d, !d->batch_mode);
			break;
		}
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const char *opt_remote = NULL;
	static const struct option long_options[] = {
		{ "batch-mode",	no_argument,		NULL, 'b' },
		{ "iterations",	required_argument,	NULL, 'n' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ NULL, 0, NULL, 0}
	};
	int c;
	struct timespec value, interval;
	struct node *n;

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	data.iterations = -1;

	spa_list_init(&data.node_list);

	while ((c = getopt_long(argc, argv, "hVr:o:bn:", long_options, NULL)) != -1) {
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
			opt_remote = optarg;
			break;
		case 'b':
			data.batch_mode = 1;
			break;
		case 'n':
			spa_atoi32(optarg, &data.iterations, 10);
			break;
		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "Can't create data loop: %m\n");
		return -1;
	}

	if (!data.batch_mode)
		data.iterations = -1;

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "Can't create context: %m\n");
		return -1;
	}

	pw_context_load_module(data.context, PW_EXTENSION_MODULE_PROFILER, NULL, NULL);

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote ? opt_remote :
					("[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]"),
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "Can't connect: %m\n");
		return -1;
	}

	pw_core_add_listener(data.core,
				   &data.core_listener,
				   &core_events, &data);
	data.registry = pw_core_get_registry(data.core,
					  PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
				       &data.registry_listener,
				       &registry_events, &data);

	data.check_profiler = pw_core_sync(data.core, 0, 0);

	if (!data.batch_mode) {
		terminal_start();
		data.win = newwin(LINES, COLS, 0, 0);
	}

	data.timer = pw_loop_add_timer(l, do_timeout, &data);
	value.tv_sec = 1;
	value.tv_nsec = 0;
	interval.tv_sec = 1;
	interval.tv_nsec = 0;
	pw_loop_update_timer(l, data.timer, &value, &interval, false);

	if (!data.batch_mode)
		pw_loop_add_io(l, fileno(stdin), SPA_IO_IN, false, do_handle_io, &data);

	pw_main_loop_run(data.loop);

	if (!data.batch_mode)
		terminal_stop();

	spa_list_consume(n, &data.node_list, link)
		remove_node(&data, n);
	if (data.profiler) {
		spa_hook_remove(&data.profiler_listener);
		pw_proxy_destroy((struct pw_proxy*)data.profiler);
	}
	spa_hook_remove(&data.registry_listener);
	pw_proxy_destroy((struct pw_proxy*)data.registry);
	spa_hook_remove(&data.core_listener);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	pw_deinit();

	return 0;
}
