/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2
#define DEFAULT_CHANNEL_MAP	"[ FL, FR ]"

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	const char *opt_group_name;
	const char *opt_channel_map;

	uint32_t channels;
	uint32_t latency;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void module_destroy(void *data)
{
	struct data *d = data;
	spa_hook_remove(&d->module_listener);
	d->module = NULL;
	pw_main_loop_quit(d->loop);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};


static void show_help(struct data *data, const char *name, bool error)
{
        fprintf(error ? stderr : stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -g, --group                           Node group (default '%s')\n"
		"  -c, --channels                        Number of channels (default %d)\n"
		"  -m, --channel-map                     Channel map (default '%s')\n"
		"  -l, --latency                         Desired latency in ms\n"
		"  -C  --capture                         Capture source to connect to\n"
		"      --capture-props                   Capture stream properties\n"
		"  -P  --playback                        Playback sink to connect to\n"
		"      --playback-props                  Playback stream properties\n",
		name,
		data->opt_group_name,
		data->channels,
		data->opt_channel_map);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const char *opt_remote = NULL;
	char cname[256];
	char *args;
	size_t size;
	FILE *f;
	static const struct option long_options[] = {
		{ "help",		no_argument,		NULL, 'h' },
		{ "version",		no_argument,		NULL, 'V' },
		{ "remote",		required_argument,	NULL, 'r' },
		{ "group",		required_argument,	NULL, 'g' },
		{ "channels",		required_argument,	NULL, 'c' },
		{ "latency",		required_argument,	NULL, 'l' },
		{ "capture",		required_argument,	NULL, 'C' },
		{ "playback",		required_argument,	NULL, 'P' },
		{ "capture-props",	required_argument,	NULL, 'i' },
		{ "playback-props",	required_argument,	NULL, 'o' },
		{ NULL, 0, NULL, 0}
	};
	int c, res = -1;

	pw_init(&argc, &argv);

	data.channels = DEFAULT_CHANNELS;
	data.opt_channel_map = DEFAULT_CHANNEL_MAP;
	data.opt_group_name = pw_get_client_name();
	if (snprintf(cname, sizeof(cname), "%s-%zd", argv[0], (size_t) getpid()) > 0)
		data.opt_group_name = cname;

	data.capture_props = pw_properties_new(NULL, NULL);
	data.playback_props = pw_properties_new(NULL, NULL);
	if (data.capture_props == NULL || data.playback_props == NULL) {
		fprintf(stderr, "can't create properties: %m\n");
		goto exit;
	}

	while ((c = getopt_long(argc, argv, "hVr:g:c:m:l:C:P:i:o:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0], false);
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
		case 'g':
			data.opt_group_name = optarg;
			break;
		case 'c':
			data.channels = atoi(optarg);
			break;
		case 'm':
			data.opt_channel_map = optarg;
			break;
		case 'l':
			data.latency = atoi(optarg) * DEFAULT_RATE / SPA_MSEC_PER_SEC;
			break;
		case 'C':
			pw_properties_set(data.capture_props, PW_KEY_NODE_TARGET, optarg);
			break;
		case 'P':
			pw_properties_set(data.playback_props, PW_KEY_NODE_TARGET, optarg);
			break;
		case 'i':
			pw_properties_update_string(data.capture_props, optarg, strlen(optarg));
			break;
		case 'o':
			pw_properties_update_string(data.playback_props, optarg, strlen(optarg));
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create main loop: %m\n");
		goto exit;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		goto exit;
	}


        if ((f = open_memstream(&args, &size)) == NULL) {
		fprintf(stderr, "can't open memstream: %m\n");
		goto exit;
	}

	fprintf(f, "{");

	if (opt_remote != NULL)
		fprintf(f, " remote.name = \"%s\"", opt_remote);
	if (data.latency != 0)
		fprintf(f, " node.latency = %u/%u", data.latency, DEFAULT_RATE);
	if (data.channels != 0)
		fprintf(f, " audio.channels = %u", data.channels);
	if (data.opt_channel_map != NULL)
		fprintf(f, " audio.position = %s", data.opt_channel_map);

	if (data.opt_group_name != NULL) {
		pw_properties_set(data.capture_props, PW_KEY_NODE_GROUP, data.opt_group_name);
		pw_properties_set(data.playback_props, PW_KEY_NODE_GROUP, data.opt_group_name);
	}

	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data.capture_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data.playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	pw_log_info("loading module with %s", args);

	data.module = pw_context_load_module(data.context,
			"libpipewire-module-loopback", args,
			NULL);
	free(args);

	if (data.module == NULL) {
		fprintf(stderr, "can't load module: %m\n");
		goto exit;
	}

	pw_impl_module_add_listener(data.module,
			&data.module_listener, &module_events, &data);

	pw_main_loop_run(data.loop);

	res = 0;
exit:
	if (data.module)
		pw_impl_module_destroy(data.module);
	if (data.context)
		pw_context_destroy(data.context);
	if (data.loop)
		pw_main_loop_destroy(data.loop);
	pw_properties_free(data.capture_props);
	pw_properties_free(data.playback_props);
	pw_deinit();

	return res;
}
