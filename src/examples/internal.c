/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 In process pipewire graph
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#include <spa/utils/names.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

struct data {
	struct pw_main_loop *loop;

	struct pw_context *context;
	struct pw_core *core;

	struct pw_proxy *source;
	struct pw_proxy *sink;
	struct pw_proxy *link;

	int res;
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	struct pw_properties *props;
	const char *dev = "hw:0";

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	if (argc > 1)
		dev = argv[1];

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop),
			pw_properties_new(
				PW_KEY_CONFIG_NAME, "client-rt.conf",
				NULL), 0);

	pw_context_load_module(data.context, "libpipewire-module-spa-node-factory", NULL, NULL);
	pw_context_load_module(data.context, "libpipewire-module-link-factory", NULL, NULL);

	data.core = pw_context_connect_self(data.context, NULL, 0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		data.res = -errno;
		goto cleanup;
	}

	props = pw_properties_new(
                        SPA_KEY_LIBRARY_NAME, "audiotestsrc/libspa-audiotestsrc",
                        SPA_KEY_FACTORY_NAME, "audiotestsrc",
                        PW_KEY_NODE_NAME, "test_source",
			"Spa:Pod:Object:Param:Props:live", "false",
                        NULL);
	data.source = pw_core_create_object(data.core,
			"spa-node-factory",
			PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE,
			&props->dict, 0);
	pw_properties_free(props);

	props = pw_properties_new(
                        SPA_KEY_LIBRARY_NAME, "alsa/libspa-alsa",
                        SPA_KEY_FACTORY_NAME, SPA_NAME_API_ALSA_PCM_SINK,
                        PW_KEY_NODE_NAME, "alsa_sink",
			"api.alsa.path", dev,
			"priority.driver", "1000",
                        NULL);
	data.sink = pw_core_create_object(data.core,
			"spa-node-factory",
			PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE,
			&props->dict, 0);

	while (true) {
		if (pw_proxy_get_bound_id(data.source) != SPA_ID_INVALID &&
		    pw_proxy_get_bound_id(data.sink) != SPA_ID_INVALID)
			break;

		pw_loop_iterate(pw_main_loop_get_loop(data.loop), -1);
        }

	pw_properties_clear(props);
	pw_properties_setf(props,
                        PW_KEY_LINK_OUTPUT_NODE, "%d", pw_proxy_get_bound_id(data.source));
        pw_properties_setf(props,
                        PW_KEY_LINK_INPUT_NODE, "%d", pw_proxy_get_bound_id(data.sink));

	data.link = pw_core_create_object(data.core,
			"link-factory",
			PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK,
			&props->dict, 0);
        pw_properties_free(props);

	pw_main_loop_run(data.loop);

cleanup:
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return data.res;
}
