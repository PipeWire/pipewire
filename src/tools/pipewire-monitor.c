/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>
#include <pipewire/sig.h>

struct data {
	bool running;
	struct pw_loop *loop;
	struct pw_core *core;
	struct pw_remote *remote;
	struct pw_proxy *registry_proxy;

	struct pw_listener on_info_changed;
	struct pw_listener on_state_changed;
};

struct proxy_data {
	void *info;
};

static void print_properties(struct spa_dict *props, char mark)
{
	struct spa_dict_item *item;

	if (props == NULL)
		return;

	printf("%c\tproperties:\n", mark);
	spa_dict_for_each(item, props) {
		printf("%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

#define MARK_CHANGE(f) ((print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void on_info_changed(struct pw_listener *listener, struct pw_remote *remote)
{
	struct pw_core_info *info = remote->info;
	bool print_all = true, print_mark = false;

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Core);
	if (print_all) {
		printf("%c\tuser-name: \"%s\"\n", MARK_CHANGE(0), info->user_name);
		printf("%c\thost-name: \"%s\"\n", MARK_CHANGE(1), info->host_name);
		printf("%c\tversion: \"%s\"\n", MARK_CHANGE(2), info->version);
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(3), info->name);
		printf("%c\tcookie: %u\n", MARK_CHANGE(4), info->cookie);
		print_properties(info->props, MARK_CHANGE(5));
	}
}

static void module_event_info(void *object, struct pw_module_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = proxy->user_data;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	info = data->info = pw_module_info_update(data->info, info);

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Module);
	if (print_all) {
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tfilename: \"%s\"\n", MARK_CHANGE(1), info->filename);
		printf("%c\targs: \"%s\"\n", MARK_CHANGE(2), info->args);
		print_properties(info->props, MARK_CHANGE(3));
	}
}

static const struct pw_module_events module_events = {
        &module_event_info,
};

static void node_event_info(void *object, struct pw_node_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = proxy->user_data;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

	info = data->info = pw_node_info_update(data->info, info);

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Node);
	if (print_all) {
		int i;

		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tinput ports: %u/%u\n", MARK_CHANGE(1), info->n_input_ports, info->max_input_ports);
		printf("%c\tinput formats:\n", MARK_CHANGE(2));
		for (i = 0; i < info->n_input_formats; i++)
			spa_debug_format(info->input_formats[i]);

		printf("%c\toutput ports: %u/%u\n", MARK_CHANGE(3), info->n_output_ports, info->max_output_ports);
		printf("%c\toutput formats:\n", MARK_CHANGE(4));
		for (i = 0; i < info->n_output_formats; i++)
			spa_debug_format(info->output_formats[i]);

		printf("%c\tstate: \"%s\"", MARK_CHANGE(5), pw_node_state_as_string(info->state));
		if (info->state == PW_NODE_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		print_properties(info->props, MARK_CHANGE(6));
	}
}

static const struct pw_node_events node_events = {
        &node_event_info
};

static void client_event_info(void *object, struct pw_client_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = proxy->user_data;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_client_info_update(data->info, info);

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Client);
	if (print_all) {
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static const struct pw_client_events client_events = {
        &client_event_info
};

static void link_event_info(void *object, struct pw_link_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = proxy->user_data;
	bool print_all, print_mark;

	print_all = true;
        if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
        else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_link_info_update(data->info, info);

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Link);
	if (print_all) {
		printf("%c\toutput-node-id: %u\n", MARK_CHANGE(0), info->output_node_id);
		printf("%c\toutput-port-id: %u\n", MARK_CHANGE(1), info->output_port_id);
		printf("%c\tinput-node-id: %u\n", MARK_CHANGE(2), info->input_node_id);
		printf("%c\tinput-port-id: %u\n", MARK_CHANGE(3), info->input_port_id);
		printf("%c\tformat:\n", MARK_CHANGE(4));
		if (info->format)
			spa_debug_format(info->format);
		else
			printf("\t  none\n");
	}
}

static const struct pw_link_events link_events = {
        &link_event_info
};

static void
destroy_proxy (void *data)
{
        struct pw_proxy *proxy = data;
        struct proxy_data *user_data = proxy->user_data;
	struct pw_core *core = proxy->remote->core;

        if (user_data->info == NULL)
                return;

        if (proxy->type == core->type.core) {
                pw_core_info_free (user_data->info);
        } else if (proxy->type == core->type.node) {
                pw_node_info_free (user_data->info);
        } else if (proxy->type == core->type.module) {
                pw_module_info_free (user_data->info);
        } else if (proxy->type == core->type.client) {
                pw_client_info_free (user_data->info);
        } else if (proxy->type == core->type.link) {
                pw_link_info_free (user_data->info);
        }
        user_data->info = NULL;
}


static void registry_event_global(void *object, uint32_t id, uint32_t type, uint32_t version)
{
        struct pw_proxy *registry_proxy = object;
        struct data *data = registry_proxy->object;
        struct pw_core *core = data->core;
        struct pw_remote *remote = registry_proxy->remote;
        struct pw_proxy *proxy = NULL;
        uint32_t client_version;
        const void *implementation;

        if (type == core->type.node) {
                implementation = &node_events;
                client_version = PW_VERSION_NODE;
	}
	else if (type == core->type.module) {
                implementation = &module_events;
                client_version = PW_VERSION_MODULE;
	}
	else if (type == core->type.client) {
                implementation = &client_events;
                client_version = PW_VERSION_CLIENT;
	}
	else if (type == core->type.link) {
                implementation = &link_events;
                client_version = PW_VERSION_LINK;
        } else
                return;

        proxy = pw_proxy_new(remote, SPA_ID_INVALID, type, sizeof(struct proxy_data));
        if (proxy == NULL)
                goto no_mem;

        pw_proxy_set_implementation(proxy, data, client_version, implementation, destroy_proxy);

        pw_registry_do_bind(registry_proxy, id, version, proxy->id);

        return;

      no_mem:
        printf("failed to create proxy");
        return;
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	printf("removed:\n");
	printf("\tid: %u\n", id);
}

static const struct pw_registry_events registry_events = {
	registry_event_global,
	registry_event_global_remove,
};

static void on_state_changed(struct pw_listener *listener, struct pw_remote *remote)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_state_changed);

	switch (remote->state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", remote->error);
		data->running = false;
		break;

	case PW_REMOTE_STATE_CONNECTED:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(remote->state));

		data->registry_proxy = pw_proxy_new(data->remote,
						    SPA_ID_INVALID,
						    data->core->type.registry,
						    0);
		pw_proxy_set_implementation(data->registry_proxy, data, PW_VERSION_REGISTRY,
                                            &registry_events, NULL);

		pw_core_do_get_registry(data->remote->core_proxy, PW_VERSION_REGISTRY,
					data->registry_proxy->id);

		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(remote->state));
		break;
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.core = pw_core_new(data.loop, NULL);
	data.remote = pw_remote_new(data.core, NULL);

	pw_signal_add(&data.remote->info_changed, &data.on_info_changed, on_info_changed);
	pw_signal_add(&data.remote->state_changed, &data.on_state_changed, on_state_changed);

	pw_remote_connect(data.remote);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_remote_destroy(data.remote);
	pw_loop_destroy(data.loop);

	return 0;
}
