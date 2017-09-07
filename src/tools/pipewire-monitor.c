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
#include <pipewire/interfaces.h>
#include <pipewire/type.h>

struct data {
	bool running;
	struct pw_loop *loop;
	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;
};

struct proxy_data {
	struct pw_proxy *proxy;
	uint32_t id;
	uint32_t parent_id;
	uint32_t permissions;
	uint32_t version;
	uint32_t type;
	void *info;
	pw_destroy_t destroy;
	struct spa_hook proxy_listener;
	struct spa_hook proxy_proxy_listener;
};

static void print_properties(struct spa_dict *props, char mark)
{
	const struct spa_dict_item *item;

	printf("%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		printf("\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		printf("%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

#define MARK_CHANGE(f) ((print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void on_info_changed(void *data, const struct pw_core_info *info)
{
	bool print_all = true, print_mark = false;

	printf("\ttype: %s\n", PW_TYPE_INTERFACE__Core);
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
        struct proxy_data *data = pw_proxy_get_user_data(proxy);
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

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n", PW_TYPE_INTERFACE__Module, data->version);
	if (print_all) {
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tfilename: \"%s\"\n", MARK_CHANGE(1), info->filename);
		printf("%c\targs: \"%s\"\n", MARK_CHANGE(2), info->args);
		print_properties(info->props, MARK_CHANGE(3));
	}
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
        .info = module_event_info,
};

static void node_event_info(void *object, struct pw_node_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = pw_proxy_get_user_data(proxy);
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

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n", PW_TYPE_INTERFACE__Node, data->version);
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

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
        .info = node_event_info
};

static void client_event_info(void *object, struct pw_client_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = pw_proxy_get_user_data(proxy);
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

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n", PW_TYPE_INTERFACE__Client, data->version);
	if (print_all) {
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
        .info = client_event_info
};

static void link_event_info(void *object, struct pw_link_info *info)
{
        struct pw_proxy *proxy = object;
        struct proxy_data *data = pw_proxy_get_user_data(proxy);
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

	printf("\tid: %d\n", data->id);
	printf("\tparent_id: %d\n", data->parent_id);
	printf("\tpermissions: %c%c%c\n", data->permissions & PW_PERM_R ? 'r' : '-',
					  data->permissions & PW_PERM_W ? 'w' : '-',
					  data->permissions & PW_PERM_X ? 'x' : '-');
	printf("\ttype: %s (version %d)\n", PW_TYPE_INTERFACE__Link, data->version);
	if (print_all) {
		printf("%c\toutput-node-id: %u\n", MARK_CHANGE(0), info->output_node_id);
		printf("%c\toutput-port-id: %u\n", MARK_CHANGE(0), info->output_port_id);
		printf("%c\tinput-node-id: %u\n", MARK_CHANGE(1), info->input_node_id);
		printf("%c\tinput-port-id: %u\n", MARK_CHANGE(1), info->input_port_id);
		printf("%c\tformat:\n", MARK_CHANGE(2));
		if (info->format)
			spa_debug_format(info->format);
		else
			printf("\t\tnone\n");
		print_properties(info->props, MARK_CHANGE(3));
	}
}

static const struct pw_link_proxy_events link_events = {
	PW_VERSION_LINK_PROXY_EVENTS,
	.info = link_event_info
};

static void
destroy_proxy (void *data)
{
        struct proxy_data *user_data = data;

        if (user_data->info == NULL)
                return;

	if (user_data->destroy)
		user_data->destroy(user_data->info);
        user_data->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = destroy_proxy,
};

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
				  uint32_t permissions, uint32_t type, uint32_t version)
{
        struct data *d = data;
        struct pw_proxy *proxy;
        uint32_t client_version;
        const void *events;
	struct pw_core *core = d->core;
	struct pw_type *t = pw_core_get_type(core);
	struct proxy_data *pd;
	pw_destroy_t destroy;

	if (type == t->node) {
		events = &node_events;
		client_version = PW_VERSION_NODE;
		destroy = (pw_destroy_t) pw_node_info_free;
	}
	else if (type == t->module) {
		events = &module_events;
		client_version = PW_VERSION_MODULE;
		destroy = (pw_destroy_t) pw_module_info_free;
	}
	else if (type == t->client) {
		events = &client_events;
		client_version = PW_VERSION_CLIENT;
		destroy = (pw_destroy_t) pw_client_info_free;
	}
	else if (type == t->link) {
		events = &link_events;
		client_version = PW_VERSION_LINK;
		destroy = (pw_destroy_t) pw_link_info_free;
	}
	else {
		printf("added:\n");
		printf("\tid: %u\n", id);
		printf("\tparent_id: %d\n", parent_id);
		printf("\tpermissions: %c%c%c\n", permissions & PW_PERM_R ? 'r' : '-',
						  permissions & PW_PERM_W ? 'w' : '-',
						  permissions & PW_PERM_X ? 'x' : '-');
		printf("\ttype: %s (version %d)\n", spa_type_map_get_type(t->map, type), version);
		return;
	}

        proxy = pw_registry_proxy_bind(d->registry_proxy, id, type,
				       client_version,
				       sizeof(struct proxy_data));
        if (proxy == NULL)
                goto no_mem;

	pd = pw_proxy_get_user_data(proxy);
	pd->proxy = proxy;
	pd->id = id;
	pd->parent_id = parent_id;
	pd->permissions = permissions;
	pd->version = version;
	pd->destroy = destroy;
        pw_proxy_add_proxy_listener(proxy, &pd->proxy_proxy_listener, events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

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

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_state_changed(void *_data, enum pw_remote_state old,
			     enum pw_remote_state state, const char *error)
{
	struct data *data = _data;
	struct pw_type *t = pw_core_get_type(data->core);

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		data->running = false;
		break;

	case PW_REMOTE_STATE_CONNECTED:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));

		data->core_proxy = pw_remote_get_core_proxy(data->remote);
		data->registry_proxy = pw_core_proxy_get_registry(data->core_proxy,
								  t->registry,
								  PW_VERSION_REGISTRY, 0);
		pw_registry_proxy_add_listener(data->registry_proxy,
					       &data->registry_listener,
					       &registry_events, data);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.info_changed = on_info_changed,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct data data = { 0 };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new(NULL);
	data.running = true;
	data.core = pw_core_new(data.loop, NULL);
	data.remote = pw_remote_new(data.core, NULL);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);
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
