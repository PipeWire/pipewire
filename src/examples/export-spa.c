/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <sys/mman.h>
#include <signal.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>

#include <pipewire/pipewire.h>
#include <pipewire/factory.h>

struct data {
	struct pw_main_loop *loop;

	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_node *node;
	const char *library;
	const char *factory;
	const char *path;
};

static int make_node(struct data *data)
{
	struct pw_factory *factory;
	struct pw_properties *props;

        factory = pw_core_find_factory(data->core, "spa-node-factory");
	if (factory == NULL)
		return -1;

        props = pw_properties_new("spa.library.name", data->library,
                                  "spa.factory.name", data->factory, NULL);

	if (data->path) {
		pw_properties_set(props, PW_NODE_PROP_AUTOCONNECT, "1");
		pw_properties_set(props, PW_NODE_PROP_TARGET_NODE, data->path);
	}

        data->node = pw_factory_create_object(factory,
					      NULL,
					      PW_TYPE_INTERFACE_Node,
					      PW_VERSION_NODE,
					      props, SPA_ID_INVALID);

	pw_node_set_active(data->node, true);

	pw_remote_export(data->remote, data->node);

	return 0;
}

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		if (make_node(data) < 0) {
			pw_log_error("can't make node");
			pw_main_loop_quit(data->loop);
		}
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

static void do_quit(void *data, int signal_number)
{
        struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	struct pw_loop *l;

	pw_init(&argc, &argv);

	if (argc < 3) {
		fprintf(stderr, "usage: %s <library> <factory> [path]\n\n"
				"\texample: %s v4l2/libspa-v4l2 v4l2-source\n\n",
				argv[0], argv[0]);
		return -1;
	}

	data.loop = pw_main_loop_new(NULL);
	l = pw_main_loop_get_loop(data.loop);
        pw_loop_add_signal(l, SIGINT, do_quit, &data);
        pw_loop_add_signal(l, SIGTERM, do_quit, &data);
	data.core = pw_core_new(l, NULL);
        data.remote = pw_remote_new(data.core, NULL, 0);
	data.library = argv[1];
	data.factory = argv[2];
	if (argc > 3)
		data.path = argv[3];

	pw_module_load(data.core, "libpipewire-module-spa-node-factory", NULL, NULL, NULL, NULL);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

        pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
