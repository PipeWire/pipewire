/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include <pipewire/control.h>
#include <pipewire/private.h>

struct impl {
	struct pw_control this;

	struct pw_memblock *mem;
};

struct pw_control *
pw_control_new(struct pw_core *core,
	       struct pw_port *port,
	       uint32_t id, uint32_t size,
	       size_t user_data_size)
{
	struct impl *impl;
	struct pw_control *this;
	enum spa_direction direction;

	switch (id) {
	case SPA_IO_Control:
		direction = SPA_DIRECTION_INPUT;
		break;
	case SPA_IO_Notify:
		direction = SPA_DIRECTION_OUTPUT;
		break;
	default:
		goto exit;
	}

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto exit;

	this = &impl->this;
	this->id = id;
	this->size = size;

	pw_log_debug("control %p: new %s %d", this,
			spa_debug_type_find_name(spa_type_io, this->id), direction);

	this->core = core;
	this->port = port;
	this->direction = direction;

	spa_list_init(&this->inputs);

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_hook_list_init(&this->listener_list);

	spa_list_append(&core->control_list[direction], &this->link);
	if (port) {
		spa_list_append(&port->control_list[direction], &this->port_link);
		pw_port_events_control_added(port, this);
	}

	return this;

    exit:
	return NULL;
}

void pw_control_destroy(struct pw_control *control)
{
	struct impl *impl = SPA_CONTAINER_OF(control, struct impl, this);
	struct pw_control *other, *tmp;

	pw_log_debug("control %p: destroy", control);

	pw_control_events_destroy(control);

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
		pw_port_events_control_removed(control->port, control);
	}

	pw_log_debug("control %p: free", control);
	pw_control_events_free(control);

	if (control->direction == SPA_DIRECTION_OUTPUT) {
		if (impl->mem)
			pw_memblock_free(impl->mem);
	}
	free(control);
}

SPA_EXPORT
struct pw_port *pw_control_get_port(struct pw_control *control)
{
	return control->port;
}

SPA_EXPORT
void pw_control_add_listener(struct pw_control *control,
			     struct spa_hook *listener,
			     const struct pw_control_events *events,
			     void *data)
{
	spa_hook_list_append(&control->listener_list, listener, events, data);
}

SPA_EXPORT
int pw_control_link(struct pw_control *control, struct pw_control *other)
{
	int res = 0;
	struct impl *impl;
	uint32_t size;

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
			spa_debug_type_find_name(spa_type_io, control->id));

	size = SPA_MAX(control->size, other->size);

	if (impl->mem == NULL) {
		if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
					     PW_MEMBLOCK_FLAG_SEAL |
					     PW_MEMBLOCK_FLAG_MAP_READWRITE,
					     size,
					     &impl->mem)) < 0)
			goto exit;

	}

	if (other->port) {
		struct pw_port *port = other->port;
		if ((res = spa_node_port_set_io(port->node->node,
				     port->direction, port->port_id,
				     other->id,
				     impl->mem->ptr, size)) < 0) {
			pw_log_warn("control %p: set io failed %d %s", control,
					res, spa_strerror(res));
			goto exit;
		}
	}

	if (spa_list_is_empty(&control->inputs)) {
		if (control->port) {
			struct pw_port *port = control->port;
			if ((res = spa_node_port_set_io(port->node->node,
					     port->direction, port->port_id,
					     control->id,
					     impl->mem->ptr, size)) < 0) {
				pw_log_warn("control %p: set io failed %d %s", control,
					res, spa_strerror(res));
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

	pw_control_events_linked(control, other);
	pw_control_events_linked(other, control);

     exit:
	return res;
}

SPA_EXPORT
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

	pw_control_events_unlinked(control, other);
	pw_control_events_unlinked(other, control);

	return res;
}
