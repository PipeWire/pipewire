/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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
#include <signal.h>

#include <spa/utils/result.h>
#include <spa/pod/parser.h>
#include <spa/debug/pod.h>

#include <pipewire/impl.h>
#include <extensions/profiler.h>

#define MAX_NAME		128
#define MAX_FOLLOWERS		64
#define DEFAULT_FILENAME	"profiler.log"

struct follower {
	uint32_t id;
	char name[MAX_NAME];
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	const char *filename;
	FILE *output;

	int n_followers;
	struct follower followers[MAX_FOLLOWERS];
};

struct proxy_data {
	struct data *data;
	struct pw_proxy *proxy;
	struct spa_hook object_listener;
};

struct measurement {
	int64_t period;
	int32_t status;
	int64_t prev_signal;
	int64_t signal;
	int64_t awake;
	int64_t finish;
};

struct point {
	float period_usecs;
	struct measurement driver;
	struct measurement follower[MAX_FOLLOWERS];
};

static void process_driver_block(struct data *d, const struct spa_pod *pod, struct point *point)
{
	union {
		int64_t l;
		int32_t i;
		char *s;
		float f;
		double d;
		struct spa_fraction F;
	} dummy;
	struct spa_io_clock clock;

	spa_pod_parse_struct(pod,
			SPA_POD_Long(&dummy.l),
			SPA_POD_Int(&dummy.i),
			SPA_POD_String(&dummy.s),
			SPA_POD_Float(&dummy.f),
			SPA_POD_Float(&dummy.f),
			SPA_POD_Float(&dummy.f),
			SPA_POD_Long(&clock.nsec),
			SPA_POD_Fraction(&clock.rate),
			SPA_POD_Long(&clock.position),
			SPA_POD_Long(&clock.duration),
			SPA_POD_Long(&clock.delay),
			SPA_POD_Double(&clock.rate_diff),
			SPA_POD_Long(&clock.next_nsec),
			SPA_POD_Long(&point->driver.prev_signal),
			SPA_POD_Long(&point->driver.signal),
			SPA_POD_Long(&point->driver.awake),
			SPA_POD_Long(&point->driver.finish),
			SPA_POD_Int(&point->driver.status));

	point->period_usecs = clock.duration * (float)SPA_USEC_PER_SEC / (clock.rate.denom * clock.rate_diff);
}

static int find_follower(struct data *d, uint32_t id, const char *name)
{
	int i;
	for (i = 0; i < d->n_followers; i++) {
		if (d->followers[i].id == id && strcmp(d->followers[i].name, name) == 0)
			return i;
	}
	return -1;
}
static int add_follower(struct data *d, uint32_t id, const char *name)
{
	int idx = d->n_followers;

	if (idx == MAX_FOLLOWERS)
		return -1;

	d->n_followers++;

	strncpy(d->followers[idx].name, name, MAX_NAME);
	d->followers[idx].name[MAX_NAME-1] = '\0';
	d->followers[idx].id = id;

	return idx;
}

static void process_follower_block(struct data *d, const struct spa_pod *pod, struct point *point)
{
	uint32_t id;
	const char *name;
	struct measurement m;
	int idx;

	spa_zero(m);
	spa_pod_parse_struct(pod,
			SPA_POD_Int(&id),
			SPA_POD_String(&name),
			SPA_POD_Long(&m.signal),
			SPA_POD_Long(&m.awake),
			SPA_POD_Long(&m.finish),
			SPA_POD_Int(&m.status));

	if ((idx = find_follower(d, id, name)) < 0) {
		if ((idx = add_follower(d, id, name)) < 0) {
			pw_log_warn("too many followers");
			return;
		}
	}
	point->follower[idx] = m;
}

static void dump_point(struct data *d, struct point *point)
{
	int i;
	int64_t d1, d2;

	d1 = (point->driver.signal - point->driver.prev_signal) / 1000;
	d2 = (point->driver.finish - point->driver.signal) / 1000;

	if (d1 > point->period_usecs * 1.3 ||
	    d2 > point->period_usecs * 1.3)
		d1 = d2 = point->period_usecs * 1.4;

	fprintf(d->output, "%"PRIi64"\t%"PRIi64"\t", d1 > 0 ? d1 : 0, d2 > 0 ? d2 : 0);

	for (i = 0; i < MAX_FOLLOWERS; i++) {
		if (point->follower[i].status == 0) {
			fprintf(d->output, " \t \t \t \t \t \t \t");
		} else {
			int64_t d4 = (point->follower[i].signal - point->driver.signal) / 1000;
			int64_t d5 = (point->follower[i].awake - point->driver.signal) / 1000;
			int64_t d6 = (point->follower[i].finish - point->driver.signal) / 1000;

			fprintf(d->output, "%u\t%"PRIi64"\t%"PRIi64"\t%"PRIi64"\t%"PRIi64"\t%"PRIi64"\t%d\t",
					d->followers[i].id,
					d4 > 0 ? d4 : 0,
					d5 > 0 ? d5 : 0,
					d6 > 0 ? d6 : 0,
					(d5 > 0 && d4 > 0) ? d5 - d4 : 0,
					(d6 > 0 && d5 > 0) ? d6 - d5 : 0,
					point->follower[i].status);
		}
	}
	fprintf(d->output, "\n");
}

static void dump_scripts(struct data *d)
{
	FILE *out;
	int i;

	out = fopen("Timing1.plot", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timing1.plot: %m");
	} else {
		fprintf(out,
			"set output 'Timing1.svg\n"
			"set terminal svg\n"
			"set grid\n"
			"set title \"Audio driver timing\"\n"
			"set xlabel \"audio cycles\"\n"
			"set ylabel \"usec\"\n"
			"plot \"%s\" using 1 title \"Audio period\" with lines \n"
			"unset output\n", d->filename);
		fclose(out);
	}

	out = fopen("Timing2.plot", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timing2.plot: %m");
	} else {
		fprintf(out,
			"set output 'Timing2.svg\n"
			"set terminal svg\n"
			"set grid\n"
			"set title \"Driver end date\"\n"
			"set xlabel \"audio cycles\"\n"
			"set ylabel \"usec\"\n"
			"plot  \"%s\" using 2 title \"Driver end date\" with lines \n"
			"unset output\n", d->filename);
		fclose(out);
	}

	out = fopen("Timing3.plot", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timing3.plot: %m");
	} else {
		fprintf(out,
			"set output 'Timing3.svg\n"
			"set terminal svg\n"
			"set multiplot\n"
			"set grid\n"
			"set title \"Clients end date\"\n"
			"set xlabel \"audio cycles\"\n"
			"set ylabel \"usec\"\n"
			"plot "
			"\"%s\" using 1 title \"Audio period\" with lines%s",
				d->filename,
				d->n_followers > 0 ? ", " : "");

		for (i = 0; i < d->n_followers; i++) {
			fprintf(out,
				"\"%s\" using %d title \"%s/%u\" with lines%s",
					d->filename, ((i + 1) * 7) - 1,
					d->followers[i].name, d->followers[i].id,
					i+1 < d->n_followers ? ", " : "");
		}
		fprintf(out,
			"\nunset multiplot\n"
			"unset output\n");
		fclose(out);
	}

	out = fopen("Timing4.plot", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timing4.plot: %m");
	} else {
		fprintf(out,
			"set output 'Timing4.svg\n"
			"set terminal svg\n"
			"set multiplot\n"
			"set grid\n"
			"set title \"Clients scheduling latency\"\n"
			"set xlabel \"audio cycles\"\n"
			"set ylabel \"usec\"\n"
			"plot ");

		for (i = 0; i < d->n_followers; i++) {
			fprintf(out,
				"\"%s\" using %d title \"%s/%u\" with lines%s",
					d->filename, ((i + 1) * 7),
					d->followers[i].name, d->followers[i].id,
					i+1 < d->n_followers ? ", " : "");
		}
		fprintf(out,
			"\nunset multiplot\n"
			"unset output\n");
		fclose(out);
	}

	out = fopen("Timing5.plot", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timing5.plot: %m");
	} else {
		fprintf(out,
			"set output 'Timing5.svg\n"
			"set terminal svg\n"
			"set multiplot\n"
			"set grid\n"
			"set title \"Clients duration\"\n"
			"set xlabel \"audio cycles\"\n"
			"set ylabel \"usec\"\n"
			"plot ");

		for (i = 0; i < d->n_followers; i++) {
			fprintf(out,
				"\"%s\" using %d title \"%s/%u\" with lines%s",
					d->filename, ((i + 1) * 7) + 1,
					d->followers[i].name, d->followers[i].id,
					i+1 < d->n_followers ? ", " : "");
		}
		fprintf(out,
			"\nunset multiplot\n"
			"unset output\n");
		fclose(out);
	}
	out = fopen("Timings.html", "w");
	if (out == NULL) {
		pw_log_error("Can't open Timings.html: %m");
	} else {
		fprintf(out,
			"<?xml version='1.0' encoding='utf-8'?>\n"
			"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
			"\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
			"<html xmlns='http://www.w3.org/1999/xhtml' lang='en'>\n"
			"  <head>\n"
			"    <title>PipeWire profiling</title>\n"
			"    <!-- assuming that images are 600px wide -->\n"
			"    <style media='all' type='text/css'>\n"
			"    .center { margin-left:auto ; margin-right: auto; width: 650px; height: 550px }\n"
			"    </style>\n"
			"  </head>\n"
			"  <body>\n"
			"    <h2 style='text-align:center'>PipeWire profiling</h2>\n"
			"    <div class='center'><object class='center' type='image/svg+xml' data='Timing1.svg'>Timing1</object></div>"
			"    <div class='center'><object class='center' type='image/svg+xml' data='Timing2.svg'>Timing2</object></div>"
			"    <div class='center'><object class='center' type='image/svg+xml' data='Timing3.svg'>Timing3</object></div>"
			"    <div class='center'><object class='center' type='image/svg+xml' data='Timing4.svg'>Timing4</object></div>"
			"    <div class='center'><object class='center' type='image/svg+xml' data='Timing5.svg'>Timing5</object></div>"
			"  </body>\n"
			"</html>\n");
		fclose(out);
	}

	out = fopen("generate_timings.sh", "w");
	if (out == NULL) {
		pw_log_error("Can't open generate_timings.sh: %m");
	} else {
		fprintf(out,
			"gnuplot -persist Timing1.plot\n"
			"gnuplot -persist Timing2.plot\n"
			"gnuplot -persist Timing3.plot\n"
			"gnuplot -persist Timing4.plot\n"
			"gnuplot -persist Timing5.plot\n");
		fclose(out);
	}
}

static void profiler_profile(void *data, const struct spa_pod *pod)
{
        struct data *d = data;
	struct spa_pod *o;
	struct spa_pod_prop *p;
	struct point point;

	SPA_POD_STRUCT_FOREACH(pod, o) {
		if (!spa_pod_is_object_type(o, SPA_TYPE_OBJECT_Profiler))
			continue;

		spa_zero(point);
		SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)o, p) {
			switch(p->key) {
			case SPA_PROFILER_driverBlock:
				process_driver_block(d, &p->value, &point);
				break;
			case SPA_PROFILER_followerBlock:
				process_follower_block(d, &p->value, &point);
				break;
			default:
				break;
			}
		}
		dump_point(d, &point);
	}
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
	struct proxy_data *pd;

	if (strcmp(type, PW_TYPE_INTERFACE_Profiler) != 0)
		return;

        proxy = pw_registry_bind(d->registry, id, type,
			PW_VERSION_PROFILER, sizeof(struct proxy_data));
        if (proxy == NULL)
                goto no_mem;

	pd = pw_proxy_get_user_data(proxy);
	pd->data = d;
	pd->proxy = proxy;

        pw_proxy_add_object_listener(proxy, &pd->object_listener, &profiler_events, d);

        return;

no_mem:
	pw_log_error("failed to create proxy");
        return;
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);
	if (id == 0) {
		pw_main_loop_quit(data->loop);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	struct pw_properties *props = NULL;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "Can't create data loop: %m");
		return -1;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "Can't create context: %m");
		return -1;
	}

	pw_context_load_module(data.context, PW_EXTENSION_MODULE_PROFILER, NULL, NULL);

	if (argc > 1)
		props = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);

	data.core = pw_context_connect(data.context, props, 0);
	if (data.core == NULL) {
		fprintf(stderr, "Can't connect: %m");
		return -1;
	}

	data.filename = DEFAULT_FILENAME;

	data.output = fopen(data.filename, "w");
	if (data.output == NULL) {
		fprintf(stderr, "Can't open file %s: %m", data.filename);
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

	pw_main_loop_run(data.loop);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	fclose(data.output);

	dump_scripts(&data);

	return 0;
}
