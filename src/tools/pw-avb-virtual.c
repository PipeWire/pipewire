/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

/**
 * pw-avb-virtual: Create virtual AVB audio devices in the PipeWire graph.
 *
 * This tool creates virtual AVB talker/listener endpoints that appear
 * as Audio/Source and Audio/Sink nodes in the PipeWire graph (visible
 * in tools like Helvum). No AVB hardware or network access is needed —
 * the loopback transport is used for all protocol and stream operations.
 *
 * The sink node consumes audio silently (data goes nowhere).
 * The source node produces silence (no network data to receive).
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <locale.h>

#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#include "module-avb/internal.h"
#include "module-avb/stream.h"
#include "module-avb/avb-transport-loopback.h"
#include "module-avb/descriptors.h"
#include "module-avb/mrp.h"
#include "module-avb/adp.h"
#include "module-avb/acmp.h"
#include "module-avb/aecp.h"
#include "module-avb/maap.h"
#include "module-avb/mmrp.h"
#include "module-avb/msrp.h"
#include "module-avb/mvrp.h"

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;

	struct impl impl;
	struct server *server;

	const char *opt_remote;
	const char *opt_name;
	bool opt_milan;
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void on_core_error(void *data, uint32_t id, int seq,
		int res, const char *message)
{
	struct data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static struct server *create_virtual_server(struct data *data)
{
	struct impl *impl = &data->impl;
	struct server *server;
	struct stream *stream;
	uint16_t idx;
	char name_buf[256];

	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	server->ifname = strdup("virtual0");
	server->avb_mode = data->opt_milan ? AVB_MODE_MILAN_V12 : AVB_MODE_LEGACY;
	server->transport = &avb_transport_loopback;

	spa_list_append(&impl->servers, &server->link);
	spa_hook_list_init(&server->listener_list);
	spa_list_init(&server->descriptors);
	spa_list_init(&server->streams);

	if (server->transport->setup(server) < 0)
		goto error;

	server->mrp = avb_mrp_new(server);
	if (server->mrp == NULL)
		goto error;

	avb_aecp_register(server);
	server->maap = avb_maap_register(server);
	server->mmrp = avb_mmrp_register(server);
	server->msrp = avb_msrp_register(server);
	server->mvrp = avb_mvrp_register(server);
	avb_adp_register(server);
	avb_acmp_register(server);

	server->domain_attr = calloc(1, sizeof(*server->domain_attr));
	avb_msrp_attribute_new(server->msrp, server->domain_attr,
			AVB_MSRP_ATTRIBUTE_TYPE_DOMAIN);
	server->domain_attr->attr.domain.sr_class_id = AVB_MSRP_CLASS_ID_DEFAULT;
	server->domain_attr->attr.domain.sr_class_priority = AVB_MSRP_PRIORITY_DEFAULT;
	server->domain_attr->attr.domain.sr_class_vid = htons(AVB_DEFAULT_VLAN);

	avb_maap_reserve(server->maap, 1);

	init_descriptors(server);

	/* Update stream properties and activate */
	idx = 0;
	spa_list_for_each(stream, &server->streams, link) {
		if (stream->direction == SPA_DIRECTION_INPUT) {
			snprintf(name_buf, sizeof(name_buf), "%s.source.%u",
					data->opt_name, idx);
			pw_stream_update_properties(stream->stream,
				&SPA_DICT_INIT_ARRAY(((struct spa_dict_item[]) {
					{ PW_KEY_NODE_NAME, name_buf },
					{ PW_KEY_NODE_DESCRIPTION, "AVB Virtual Source" },
					{ PW_KEY_NODE_VIRTUAL, "true" },
				})));
		} else {
			snprintf(name_buf, sizeof(name_buf), "%s.sink.%u",
					data->opt_name, idx);
			pw_stream_update_properties(stream->stream,
				&SPA_DICT_INIT_ARRAY(((struct spa_dict_item[]) {
					{ PW_KEY_NODE_NAME, name_buf },
					{ PW_KEY_NODE_DESCRIPTION, "AVB Virtual Sink" },
					{ PW_KEY_NODE_VIRTUAL, "true" },
				})));
		}

		if (stream_activate_virtual(stream, idx) < 0)
			pw_log_warn("failed to activate stream %u", idx);

		idx++;
	}

	return server;

error:
	spa_list_remove(&server->link);
	free(server->ifname);
	free(server);
	return NULL;
}

static void show_help(const char *name)
{
	printf("%s [options]\n"
		"  -h, --help                Show this help\n"
		"      --version             Show version\n"
		"  -r, --remote NAME         Remote daemon name\n"
		"  -n, --name PREFIX         Node name prefix (default: avb-virtual)\n"
		"  -m, --milan               Use Milan v1.2 mode (default: legacy)\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	int res = -1;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "name",	required_argument,	NULL, 'n' },
		{ "milan",	no_argument,		NULL, 'm' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	data.opt_name = "avb-virtual";

	while ((c = getopt_long(argc, argv, "hVr:n:m", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0]);
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
		case 'n':
			data.opt_name = optarg;
			break;
		case 'm':
			data.opt_milan = true;
			break;
		default:
			show_help(argv[0]);
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

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data.opt_remote,
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect to PipeWire: %m\n");
		goto exit;
	}

	pw_core_add_listener(data.core, &data.core_listener,
			&core_events, &data);

	/* Initialize the AVB impl */
	data.impl.loop = l;
	data.impl.timer_queue = pw_context_get_timer_queue(data.context);
	data.impl.context = data.context;
	data.impl.core = data.core;
	spa_list_init(&data.impl.servers);

	/* Create the virtual AVB server with streams */
	data.server = create_virtual_server(&data);
	if (data.server == NULL) {
		fprintf(stderr, "can't create virtual AVB server: %m\n");
		goto exit;
	}

	fprintf(stdout, "Virtual AVB device running (%s mode). Press Ctrl-C to stop.\n",
			data.opt_milan ? "Milan v1.2" : "legacy");

	pw_main_loop_run(data.loop);

	res = 0;
exit:
	if (data.server)
		avdecc_server_free(data.server);
	if (data.core)
		pw_core_disconnect(data.core);
	if (data.context)
		pw_context_destroy(data.context);
	if (data.loop)
		pw_main_loop_destroy(data.loop);
	pw_deinit();

	return res;
}
