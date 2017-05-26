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

#include <pipewire/client/pipewire.h>
#include <pipewire/client/sig.h>
#include <spa/lib/debug.h>

struct data {
	bool running;
	struct pw_loop *loop;
	struct pw_context *context;

	struct pw_listener on_state_changed;
	struct pw_listener on_subscription;
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

struct dumpdata {
	bool print_mark;
	bool print_all;
};

#define MARK_CHANGE(f) ((data->print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void
dump_core_info(struct pw_context *c, int res, const struct pw_core_info *info, void *user_data)
{
	struct dumpdata *data = user_data;

	if (info == NULL)
		return;

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Core);
	if (data->print_all) {
		printf("%c\tuser-name: \"%s\"\n", MARK_CHANGE(0), info->user_name);
		printf("%c\thost-name: \"%s\"\n", MARK_CHANGE(1), info->host_name);
		printf("%c\tversion: \"%s\"\n", MARK_CHANGE(2), info->version);
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(3), info->name);
		printf("%c\tcookie: %u\n", MARK_CHANGE(4), info->cookie);
		print_properties(info->props, MARK_CHANGE(5));
	}
}

static void
dump_client_info(struct pw_context *c, int res, const struct pw_client_info *info, void *user_data)
{
	struct dumpdata *data = user_data;

	if (info == NULL)
		return;

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Client);
	if (data->print_all) {
		print_properties(info->props, MARK_CHANGE(0));
	}
}

static void
dump_node_info(struct pw_context *c, int res, const struct pw_node_info *info, void *user_data)
{
	struct dumpdata *data = user_data;

	if (info == NULL) {
		if (res != SPA_RESULT_ENUM_END)
			printf("\tError introspecting node: %d\n", res);
		return;
	}

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Node);
	if (data->print_all) {
		int i;

		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tinputs: %u/%u\n", MARK_CHANGE(1), info->n_inputs, info->max_inputs);
		printf("%c\tinput formats:\n", MARK_CHANGE(2));
		for (i = 0; i < info->n_input_formats; i++)
			spa_debug_format(info->input_formats[i], c->type.map);

		printf("%c\toutputs: %u/%u\n", MARK_CHANGE(3), info->n_outputs, info->max_outputs);
		printf("%c\toutput formats:\n", MARK_CHANGE(4));
		for (i = 0; i < info->n_output_formats; i++)
			spa_debug_format(info->output_formats[i], c->type.map);

		printf("%c\tstate: \"%s\"", MARK_CHANGE(5), pw_node_state_as_string(info->state));
		if (info->state == PW_NODE_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		print_properties(info->props, MARK_CHANGE(6));
	}
}

static void
dump_module_info(struct pw_context *c, int res, const struct pw_module_info *info, void *user_data)
{
	struct dumpdata *data = user_data;

	if (info == NULL) {
		if (res != SPA_RESULT_ENUM_END)
			printf("\tError introspecting module: %d\n", res);
		return;
	}

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Module);
	if (data->print_all) {
		printf("%c\tname: \"%s\"\n", MARK_CHANGE(0), info->name);
		printf("%c\tfilename: \"%s\"\n", MARK_CHANGE(1), info->filename);
		printf("%c\targs: \"%s\"\n", MARK_CHANGE(2), info->args);
		print_properties(info->props, MARK_CHANGE(3));
	}
}

static void
dump_link_info(struct pw_context *c, int res, const struct pw_link_info *info, void *user_data)
{
	struct dumpdata *data = user_data;

	if (info == NULL) {
		if (res != SPA_RESULT_ENUM_END)
			printf("\tError introspecting link: %d\n", res);
		return;
	}

	printf("\tid: %u\n", info->id);
	printf("\ttype: %s\n", PIPEWIRE_TYPE__Link);
	if (data->print_all) {
		printf("%c\toutput-node-id: %u\n", MARK_CHANGE(0), info->output_node_id);
		printf("%c\toutput-port-id: %u\n", MARK_CHANGE(1), info->output_port_id);
		printf("%c\tinput-node-id: %u\n", MARK_CHANGE(2), info->input_node_id);
		printf("%c\tinput-port-id: %u\n", MARK_CHANGE(3), info->input_port_id);
	}
}

static void
dump_object(struct pw_context *context, uint32_t type, uint32_t id, struct dumpdata *data)
{
	if (type == context->type.core) {
		pw_context_get_core_info(context, dump_core_info, data);
	} else if (type == context->type.node) {
		pw_context_get_node_info_by_id(context, id, dump_node_info, data);
	} else if (type == context->type.module) {
		pw_context_get_module_info_by_id(context, id, dump_module_info, data);
	} else if (type == context->type.client) {
		pw_context_get_client_info_by_id(context, id, dump_client_info, data);
	} else if (type == context->type.link) {
		pw_context_get_link_info_by_id(context, id, dump_link_info, data);
	} else {
		printf("\tid: %u\n", id);
	}


}

static void
on_subscription(struct pw_listener *listener,
		struct pw_context *context,
		enum pw_subscription_event event, uint32_t type, uint32_t id)
{
	struct dumpdata dd;

	switch (event) {
	case PW_SUBSCRIPTION_EVENT_NEW:
		printf("added:\n");
		dd.print_mark = false;
		dd.print_all = true;
		dump_object(context, type, id, &dd);
		break;

	case PW_SUBSCRIPTION_EVENT_CHANGE:
		printf("changed:\n");
		dd.print_mark = true;
		dd.print_all = true;
		dump_object(context, type, id, &dd);
		break;

	case PW_SUBSCRIPTION_EVENT_REMOVE:
		printf("removed:\n");
		dd.print_mark = false;
		dd.print_all = false;
		dump_object(context, type, id, &dd);
		break;
	}
}

static void on_state_changed(struct pw_listener *listener, struct pw_context *context)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_state_changed);

	switch (context->state) {
	case PW_CONTEXT_STATE_ERROR:
		printf("context error: %s\n", context->error);
		data->running = false;
		break;

	default:
		printf("context state: \"%s\"\n", pw_context_state_as_string(context->state));
		break;
	}
}

int main(int argc, char *argv[])
{
	struct data data;

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.context = pw_context_new(data.loop, "pipewire-monitor", NULL);

	pw_signal_add(&data.context->state_changed, &data.on_state_changed, on_state_changed);

	pw_signal_add(&data.context->subscription, &data.on_subscription, on_subscription);

	pw_context_connect(data.context, 0);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_context_destroy(data.context);
	pw_loop_destroy(data.loop);

	return 0;
}
