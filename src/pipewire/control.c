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

#include <spa/pod/parser.h>

#include <pipewire/control.h>
#include <pipewire/private.h>

struct impl {
	struct pw_control this;

	struct pw_memblock *mem;
};

struct pw_control *
pw_control_new(struct pw_core *core,
	       struct pw_port *port,
	       const struct spa_pod *param,
	       size_t user_data_size)
{
	struct impl *impl;
	struct pw_control *this;
	enum spa_direction direction;
	struct pw_type *t = &core->type;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto exit;

	this = &impl->this;

	direction = spa_pod_is_object_id(param, t->param_io.idPropsOut) ?
		SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;

	if (spa_pod_object_parse(param,
				":", t->param_io.id, "I", &this->id,
				":", t->param_io.size, "i", &this->size,
				":", t->param.propId, "I", &this->prop_id) < 0)
		goto exit_free;

	pw_log_debug("control %p: new %s %d", this,
			spa_type_map_get_type(t->map, this->prop_id), direction);

	this->core = core;
	this->port = port;
	this->param = pw_spa_pod_copy(param);
	this->direction = direction;

	spa_list_init(&this->inputs);

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_hook_list_init(&this->listener_list);

	spa_list_append(&core->control_list[direction], &this->link);
	if (port) {
		spa_list_append(&port->control_list[direction], &this->port_link);
		spa_hook_list_call(&port->listener_list, struct pw_port_events, control_added, this);
	}

	return this;

    exit_free:
	free(impl);
    exit:
	return NULL;
}

void pw_control_destroy(struct pw_control *control)
{
	struct impl *impl = SPA_CONTAINER_OF(control, struct impl, this);
	struct pw_control *other, *tmp;

	pw_log_debug("control %p: destroy", control);

	spa_hook_list_call(&control->listener_list, struct pw_control_events, destroy);

	if (control->direction == SPA_DIRECTION_OUTPUT) {
		spa_list_for_each_safe(other, tmp, &control->inputs, inputs_link)
			pw_control_unlink(control, other);
	}
	else {
		if (control->output)
			pw_control_unlink(control->output, control);
	}

	spa_list_remove(&control->link);

	if (control->port) {
		spa_list_remove(&control->port_link);
		spa_hook_list_call(&control->port->listener_list,
				struct pw_port_events, control_removed, control);
	}

	pw_log_debug("control %p: free", control);
	spa_hook_list_call(&control->listener_list, struct pw_control_events, free);

	if (control->direction == SPA_DIRECTION_OUTPUT) {
		if (impl->mem)
			pw_memblock_free(impl->mem);
	}

	free(control->param);

	free(control);
}

struct pw_port *pw_control_get_port(struct pw_control *control)
{
	return control->port;
}

void pw_control_add_listener(struct pw_control *control,
			     struct spa_hook *listener,
			     const struct pw_control_events *events,
			     void *data)
{
	spa_hook_list_append(&control->listener_list, listener, events, data);
}

int pw_control_link(struct pw_control *control, struct pw_control *other)
{
	int res = 0;
	struct impl *impl;

	if (control->direction == SPA_DIRECTION_INPUT) {
		struct pw_control *tmp = control;
		control = other;
		other = tmp;
	}
	if (control->direction != SPA_DIRECTION_OUTPUT ||
	    other->direction != SPA_DIRECTION_INPUT)
		return -EINVAL;

	/* input control already has a linked output control */
	if (other->output != NULL)
		return -EEXIST;

	impl = SPA_CONTAINER_OF(control, struct impl, this);

	pw_log_debug("control %p: link to %p %s", control, other,
			spa_type_map_get_type(control->core->type.map, control->prop_id));

	if (impl->mem == NULL) {
		if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
					     PW_MEMBLOCK_FLAG_SEAL |
					     PW_MEMBLOCK_FLAG_MAP_READWRITE,
					     control->size,
					     &impl->mem)) < 0)
			goto exit;

	}

	if (other->port) {
		struct pw_port *port = other->port;
		if ((res = spa_node_port_set_io(port->node->node,
				     port->direction, port->port_id,
				     other->id,
				     impl->mem->ptr, control->size)) < 0) {
			goto exit;
		}
	}

	if (spa_list_is_empty(&control->inputs)) {
		if (control->port) {
			struct pw_port *port = control->port;
			if ((res = spa_node_port_set_io(port->node->node,
					     port->direction, port->port_id,
					     control->id,
					     impl->mem->ptr, control->size)) < 0) {
				/* undo */
				port = other->port;
				spa_node_port_set_io(port->node->node,
	                                     port->direction, port->port_id,
	                                     other->id,
	                                     NULL, 0);
				goto exit;
			}
		}
	}

	other->output = control;
	spa_list_append(&control->inputs, &other->inputs_link);

	spa_hook_list_call(&control->listener_list, struct pw_control_events, linked, other);
	spa_hook_list_call(&other->listener_list, struct pw_control_events, linked, control);

     exit:
	return res;
}

int pw_control_unlink(struct pw_control *control, struct pw_control *other)
{
	int res = 0;

	pw_log_debug("control %p: unlink from %p", control, other);

	if (control->direction == SPA_DIRECTION_INPUT) {
		struct pw_control *tmp = control;
		control = other;
		other = tmp;
	}
	if (control->direction != SPA_DIRECTION_OUTPUT ||
	    other->direction != SPA_DIRECTION_INPUT)
		return -EINVAL;

	if (other->output != control)
		return -EINVAL;

	other->output = NULL;
	spa_list_remove(&other->inputs_link);

	if (spa_list_is_empty(&control->inputs)) {
		struct pw_port *port = control->port;
		if ((res = spa_node_port_set_io(port->node->node,
				     port->direction, port->port_id,
				     control->id, NULL, 0)) < 0) {
			pw_log_warn("control %p: can't unset port control io", control);
		}
	}

	if (other->port) {
		struct pw_port *port = other->port;
		if ((res = spa_node_port_set_io(port->node->node,
				     port->direction, port->port_id,
				     other->id, NULL, 0)) < 0) {
			pw_log_warn("control %p: can't unset port control io", control);
		}
	}

	spa_hook_list_call(&control->listener_list, struct pw_control_events, unlinked, other);
	spa_hook_list_call(&other->listener_list, struct pw_control_events, unlinked, control);

	return res;
}
