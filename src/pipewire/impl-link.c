/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <spa/node/utils.h>
#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/pod/filter.h>
#include <spa/param/param.h>
#include <spa/debug/types.h>

#define PW_API_LINK_IMPL	SPA_EXPORT
#include "pipewire/impl-link.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_link);
#define PW_LOG_TOPIC_DEFAULT log_link

#define MAX_HOPS	32

#define pw_link_resource_info(r,...)      pw_resource_call(r,struct pw_link_events,info,0,__VA_ARGS__)

struct port_info {
	uint32_t busy_id;
	int pending_seq;
	int result;
	struct spa_hook port_listener;
	struct spa_hook node_listener;
	struct spa_hook global_listener;
	struct pw_impl_port *port;
	struct pw_impl_node *node;
	struct pw_impl_port_mix *mix;
};

/** \cond */
struct impl {
	struct pw_impl_link this;

	unsigned int activated:1;

	struct pw_work_queue *work;

	struct port_info input;
	struct port_info output;

	struct spa_pod *format_filter;
	struct pw_properties *properties;

	struct spa_io_buffers io[2];
};

/** \endcond */

static void info_changed(struct pw_impl_link *link)
{
	struct pw_resource *resource;

	if (link->info.change_mask == 0)
		return;

	pw_impl_link_emit_info_changed(link, &link->info);

	if (link->global)
		spa_list_for_each(resource, &link->global->resource_list, link)
			pw_link_resource_info(resource, &link->info);

	link->info.change_mask = 0;
}

static inline int port_set_busy_id(struct pw_impl_link *link, struct port_info *info, uint32_t id, int pending_seq)
{
	int res = info->result;
	if (info->busy_id != SPA_ID_INVALID)
		info->port->busy_count--;
	if (id != SPA_ID_INVALID)
		info->port->busy_count++;
	info->busy_id = id;
	info->pending_seq = SPA_RESULT_ASYNC_SEQ(pending_seq);
	info->result = 0;
	if (info->port->busy_count < 0)
		pw_log_error("%s: invalid busy count:%d", link->name, info->port->busy_count);
	return res;
}

static void link_update_state(struct pw_impl_link *link, enum pw_link_state state, int res, char *error)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	enum pw_link_state old = link->info.state;

	link->info.state = state;
	free((char*)link->info.error);
	link->info.error = error;

	if (state == old)
		return;

	pw_log_debug("%p: %s -> %s (%s)", link,
		     pw_link_state_as_string(old),
		     pw_link_state_as_string(state), error);

	if (state == PW_LINK_STATE_ERROR) {
		pw_log_error("(%s) %s -> error %s (%d) (%s-%s)", link->name,
				pw_link_state_as_string(old), error, res,
				pw_impl_port_state_as_string(link->output->state),
				pw_impl_port_state_as_string(link->input->state));
	} else {
		pw_log_info("(%s) %s -> %s (%s-%s)", link->name,
				pw_link_state_as_string(old),
				pw_link_state_as_string(state),
				pw_impl_port_state_as_string(link->output->state),
				pw_impl_port_state_as_string(link->input->state));
	}

	pw_impl_link_emit_state_changed(link, old, state, error);

	link->info.change_mask |= PW_LINK_CHANGE_MASK_STATE;
	if (state == PW_LINK_STATE_ERROR ||
	    state == PW_LINK_STATE_PAUSED ||
	    state == PW_LINK_STATE_ACTIVE)
		info_changed(link);

	if (state == PW_LINK_STATE_ERROR && link->global) {
		struct pw_resource *resource;
		spa_list_for_each(resource, &link->global->resource_list, link)
			pw_resource_error(resource, res, error);
	}

	if (old < PW_LINK_STATE_PAUSED && state == PW_LINK_STATE_PAUSED) {
		link->prepared = true;
		link->preparing = false;
		pw_context_recalc_graph(link->context, "link prepared");
	} else if (old >= PW_LINK_STATE_PAUSED && state < PW_LINK_STATE_PAUSED) {
		link->prepared = false;
		link->preparing = false;
		pw_context_recalc_graph(link->context, "link unprepared");
	} else if (state == PW_LINK_STATE_INIT) {
		link->prepared = false;
		link->preparing = false;

		port_set_busy_id(link, &impl->output, SPA_ID_INVALID, SPA_ID_INVALID);
		pw_work_queue_cancel(impl->work, &impl->output, SPA_ID_INVALID);

		port_set_busy_id(link, &impl->input, SPA_ID_INVALID, SPA_ID_INVALID);
		pw_work_queue_cancel(impl->work, &impl->input, SPA_ID_INVALID);
	}
}

static void complete_ready(void *obj, void *data, int res, uint32_t id)
{
	struct port_info *info = obj;
	struct pw_impl_port *port = info->port;
	struct pw_impl_link *this = data;

	if (id != SPA_ID_INVALID) {
		if (id == info->busy_id)
			res = port_set_busy_id(this, info, SPA_ID_INVALID, SPA_ID_INVALID);
		else
			return;
	}

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s", this, obj, port,
			port->state, spa_strerror(res));

	if (SPA_RESULT_IS_OK(res)) {
		if (port->state < PW_IMPL_PORT_STATE_READY)
			pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_READY,
					0, NULL);
		if (this->input->state >= PW_IMPL_PORT_STATE_READY &&
		    this->output->state >= PW_IMPL_PORT_STATE_READY)
			link_update_state(this, PW_LINK_STATE_ALLOCATING, 0, NULL);
	} else {
		link_update_state(this, PW_LINK_STATE_ERROR, res, strdup("Format negotiation failed"));
	}
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct port_info *info = obj;
	struct pw_impl_port *port = info->port;
	struct pw_impl_port_mix *mix = info->mix;
	struct pw_impl_link *this = data;

	if (id != SPA_ID_INVALID) {
		if (id == info->busy_id)
			res = port_set_busy_id(this, info, SPA_ID_INVALID, SPA_ID_INVALID);
		else
			return;
	}

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s (%d)", this, obj, port,
			port->state, spa_strerror(res), res);

	if (SPA_RESULT_IS_OK(res)) {
		if (port->state < PW_IMPL_PORT_STATE_PAUSED)
			pw_impl_port_update_state(port, PW_IMPL_PORT_STATE_PAUSED,
					0, NULL);
		mix->have_buffers = true;

		if (this->rt.in_mix.have_buffers && this->rt.out_mix.have_buffers)
			link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
	} else {
		mix->have_buffers = false;
		link_update_state(this, PW_LINK_STATE_ERROR, res, strdup("Buffer allocation failed"));
	}
}

static void complete_sync(void *obj, void *data, int res, uint32_t id)
{
	struct port_info *info = obj;
	struct pw_impl_port *port = info->port;
	struct pw_impl_link *this = data;

	pw_log_debug("%p: obj:%p port %p complete state:%d: %s", this, obj, port,
			port->state, spa_strerror(res));
}

/* find a common format. info[0] has the higher priority.
 * Either the format contains a valid common format or error is set. */
static int link_find_format(struct pw_impl_link *this,
			struct port_info *info[2],
			uint32_t port_id[2],
			struct spa_pod **format,
			struct spa_pod_builder *builder,
			char **error)
{
	int res;
	uint32_t state[2];
	uint32_t idx[2] = { 0, 0 };
	struct spa_pod_builder fb = { 0 };
	uint8_t fbuf[4096];
	struct spa_pod *filter;
	struct spa_node *node[2];
	const char *dir[2];

	state[0] = info[0]->port->state;
	state[1] = info[1]->port->state;
	node[0] = info[0]->node->node;
	node[1] = info[1]->node->node;
	port_id[0] = info[0]->port->port_id;
	port_id[1] = info[1]->port->port_id;
	dir[0] = pw_direction_as_string(info[0]->port->direction);
	dir[1] = pw_direction_as_string(info[1]->port->direction);

	pw_log_debug("%p: finding best format %d %d", this, state[0], state[1]);

	/* when a port is configured but the node is idle, we can reconfigure with a different format */
	if (state[1] > PW_IMPL_PORT_STATE_CONFIGURE && info[1]->node->info.state == PW_NODE_STATE_IDLE)
		state[1] = PW_IMPL_PORT_STATE_CONFIGURE;
	if (state[0] > PW_IMPL_PORT_STATE_CONFIGURE && info[0]->node->info.state == PW_NODE_STATE_IDLE)
		state[0] = PW_IMPL_PORT_STATE_CONFIGURE;

	pw_log_debug("%p: states %d %d", this, state[0], state[1]);

	if (state[0] == PW_IMPL_PORT_STATE_CONFIGURE && state[1] > PW_IMPL_PORT_STATE_CONFIGURE) {
		/* only port 0 needs format, take format from port 1 and filter */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(node[1],
						     info[1]->port->direction, port_id[1],
						     SPA_PARAM_Format, &idx[1],
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				*error = spa_aprintf("error get %s format: %s", dir[1],
						spa_strerror(res));
			else
				*error = spa_aprintf("no %s formats", dir[1]);
			goto error;
		}
		pw_log_debug("%p: Got %s format:", this, dir[1]);
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(node[0],
						     info[0]->port->direction, port_id[0],
						     SPA_PARAM_EnumFormat, &idx[0],
						     filter, format, builder)) <= 0) {
			if (res == -ENOENT || res == 0) {
				pw_log_debug("%p: no %s format filter, using %s format: %s",
						this, dir[0], dir[1], spa_strerror(res));

				uint32_t offset = builder->state.offset;
				res = spa_pod_builder_raw_padded(builder, filter, SPA_POD_SIZE(filter));
				if (res < 0) {
					*error = spa_aprintf("failed to add pod");
					goto error;
				}

				*format = spa_pod_builder_deref(builder, offset);
			} else {
				*error = spa_aprintf("error %s enum formats: %s", dir[0],
						spa_strerror(res));
				goto error;
			}
		}
	} else if (state[1] >= PW_IMPL_PORT_STATE_CONFIGURE && state[0] > PW_IMPL_PORT_STATE_CONFIGURE) {
		/* only port 1 needs format, take and filter format from port 0 */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(node[0],
						     info[0]->port->direction, port_id[0],
						     SPA_PARAM_Format, &idx[0],
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				*error = spa_aprintf("error get %s format: %s", dir[0],
						spa_strerror(res));
			else
				*error = spa_aprintf("no %s format", dir[0]);
			goto error;
		}
		pw_log_debug("%p: Got %s format:", this, dir[0]);
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(node[1],
						     info[1]->port->direction, port_id[1],
						     SPA_PARAM_EnumFormat, &idx[1],
						     filter, format, builder)) <= 0) {
			if (res == -ENOENT || res == 0) {
				pw_log_debug("%p: no %s format filter, using %s format: %s",
						this, dir[1], dir[0], spa_strerror(res));

				uint32_t offset = builder->state.offset;
				res = spa_pod_builder_raw_padded(builder, filter, SPA_POD_SIZE(filter));
				if (res < 0) {
					*error = spa_aprintf("failed to add pod");
					goto error;
				}

				*format = spa_pod_builder_deref(builder, offset);
			} else {
				*error = spa_aprintf("error %s enum formats: %s", dir[1],
						spa_strerror(res));
				goto error;
			}
		}
	} else if (state[0] == PW_IMPL_PORT_STATE_CONFIGURE && state[1] == PW_IMPL_PORT_STATE_CONFIGURE) {
		bool do_filter = true;
		int count = 0;
	      again:
		/* both ports need a format, we start with a format from port 0 and use that
		 * as a filter for port 1. Because the filter has higher priority, its
		 * defaults will be prefered. */
		pw_log_debug("%p: do enum %s %d", this, dir[0], idx[0]);
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(node[0],
						     info[0]->port->direction, port_id[0],
						     SPA_PARAM_EnumFormat, &idx[0],
						     NULL, &filter, &fb)) != 1) {
			if (res == -ENOENT) {
				pw_log_debug("%p: no %s filter", this, dir[0]);
				filter = NULL;
			} else {
				if (res < 0)
					*error = spa_aprintf("error %s enum formats: %s", dir[0],
							spa_strerror(res));
				else if (do_filter && count > 0) {
					do_filter = false;
					idx[0] = 0;
					goto again;
				} else {
					*error = spa_aprintf("no more %s formats", dir[0]);
				}
				goto error;
			}
		}
		if (do_filter && filter)
			if ((res = spa_pod_filter_make(filter)) > 0)
				count += res;

		pw_log_debug("%p: enum %s %d with filter: %p", this, dir[1], idx[1], filter);
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, filter);

		if ((res = spa_node_port_enum_params_sync(node[1],
						     info[1]->port->direction, port_id[1],
						     SPA_PARAM_EnumFormat, &idx[1],
						     filter, format, builder)) != 1) {
			if (res == 0 && filter != NULL) {
				idx[1] = 0;
				goto again;
			}
			*error = spa_aprintf("error %s enum formats: %s", dir[1], spa_strerror(res));
			goto error;
		}

		pw_log_debug("%p: Got filtered:", this);
		pw_log_pod(SPA_LOG_LEVEL_DEBUG, *format);
	} else {
		res = -EBADF;
		*error = spa_aprintf("error bad node state");
		goto error;
	}
	return res;
error:
	if (res == 0)
		res = -EINVAL;
	return res;
}

static int do_negotiate(struct pw_impl_link *this)
{
	struct pw_context *context = this->context;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = -EIO, res2;
	struct spa_pod *format = NULL, *current;
	char *error = NULL;
	bool changed = false;
	struct port_info *info[2];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t index, busy_id;
	uint32_t state[2];
	struct spa_node *node[2];
	uint32_t port_id[2];
	const char *dir[2];

	if (this->info.state >= PW_LINK_STATE_NEGOTIATING)
		return 0;

	/* driver nodes have lower priority for selecting the format.
	 * Higher priority nodes go into info[0] */
	if (this->output->node->driver) {
		info[0] = &impl->input;
		info[1] = &impl->output;
	} else {
		info[0] = &impl->output;
		info[1] = &impl->input;
	}
	state[0] = info[0]->port->state;
	state[1] = info[1]->port->state;

	dir[0] = pw_direction_as_string(info[0]->port->direction),
	dir[1] = pw_direction_as_string(info[1]->port->direction),

	pw_log_info("%p: %s:%d -> %s:%d", this, dir[0], state[0], dir[1], state[1]);

	if (state[0] != PW_IMPL_PORT_STATE_CONFIGURE && state[1] != PW_IMPL_PORT_STATE_CONFIGURE)
		return 0;

	link_update_state(this, PW_LINK_STATE_NEGOTIATING, 0, NULL);

#if 0
	node[0] = info[0]->port->mix;
	port_id[0] = this->rt.in_mix.port.port_id;
	node[1] = info[1]->port->mix;
	port_id[1] = this->rt.out_mix.port.port_id;
#else
	node[0] = info[0]->node->node;
	port_id[0] = info[0]->port->port_id;
	node[1] = info[1]->node->node;
	port_id[1] = info[1]->port->port_id;
#endif

	/* find a common format for the ports */
	if ((res = link_find_format(this, info, port_id, &format, &b, &error)) < 0) {
		format = NULL;
		goto error;
	}

	format = spa_pod_copy(format);
	pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);
	spa_pod_fixate(format);
	pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	/* if port 1 had format and is idle, check if it changed. If so, renegotiate */
	if (state[1] > PW_IMPL_PORT_STATE_CONFIGURE && info[1]->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(node[1],
				info[1]->port->direction, port_id[1],
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			SPA_FALLTHROUGH
		case 1:
			break;
		case 0:
			res = -EBADF;
			SPA_FALLTHROUGH
		default:
			error = spa_aprintf("error get %s format: %s",
					pw_direction_as_string(info[1]->port->direction),
					spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("%p: %s format change, renegotiate", this,
					pw_direction_as_string(info[1]->port->direction));
			if (current)
				pw_log_pod(SPA_LOG_LEVEL_DEBUG, current);
			pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);
			pw_impl_node_set_state(info[1]->node, PW_NODE_STATE_SUSPENDED);
			state[1] = PW_IMPL_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("%p: format was already set", this);
		}
	}
	/* if port 0 had format and is idle, check if it changed. If so, renegotiate */
	if (state[0] > PW_IMPL_PORT_STATE_CONFIGURE && info[0]->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(node[0],
				info[0]->port->direction, port_id[0],
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			SPA_FALLTHROUGH
		case 1:
			break;
		case 0:
			res = -EBADF;
			SPA_FALLTHROUGH
		default:
			error = spa_aprintf("error get %s format: %s",
					pw_direction_as_string(info[0]->port->direction),
					spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("%p: %s format change, renegotiate", this,
					pw_direction_as_string(info[0]->port->direction));
			if (current)
				pw_log_pod(SPA_LOG_LEVEL_DEBUG, current);
			pw_log_pod(SPA_LOG_LEVEL_DEBUG, format);
			pw_impl_node_set_state(info[0]->node, PW_NODE_STATE_SUSPENDED);
			state[0] = PW_IMPL_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("%p: format was already set", this);
		}
	}

	SPA_POD_OBJECT_ID(format) = SPA_PARAM_Format;
	pw_log_debug("%p: doing set format %p fixated:%d", this,
			format, spa_pod_is_fixated(format));
	pw_log_pod(SPA_LOG_LEVEL_INFO, format);

	if (state[1] == PW_IMPL_PORT_STATE_CONFIGURE) {
		pw_log_debug("%p: doing set format on %s", this, dir[1]);
		if ((res = pw_impl_port_set_param(info[1]->port,
						SPA_PARAM_Format, 0,
						format)) < 0) {
			error = spa_aprintf("error set %s format: %d (%s)", dir[1],
					res, spa_strerror(res));
			pw_log_error("tried to set %s format:", dir[1]);
			pw_log_pod(SPA_LOG_LEVEL_ERROR, format);
			goto error;
		}
		pw_log_debug("%s set format: %d", dir[1], res);
		if (SPA_RESULT_IS_ASYNC(res)) {
			busy_id = pw_work_queue_add(impl->work, info[1],
					spa_node_sync(info[1]->node->node, res),
					complete_ready, this);
			port_set_busy_id(this, info[1], busy_id, res);
		} else {
			complete_ready(info[1], this, res, SPA_ID_INVALID);
		}
		changed = true;
	}
	if (state[0] == PW_IMPL_PORT_STATE_CONFIGURE) {
		pw_log_debug("%p: doing set format on %s", this, dir[0]);
		if ((res2 = pw_impl_port_set_param(info[0]->port,
						SPA_PARAM_Format, 0,
						format)) < 0) {
			error = spa_aprintf("error set %s format: %d (%s)", dir[0],
					res2, spa_strerror(res2));
			pw_log_error("tried to set %s format:", dir[0]);
			pw_log_pod(SPA_LOG_LEVEL_ERROR, format);
			goto error;
		}
		pw_log_debug("%s set format: %d", dir[0], res2);
		if (SPA_RESULT_IS_ASYNC(res2)) {
			busy_id = pw_work_queue_add(impl->work, info[0],
					spa_node_sync(info[0]->node->node, res2),
					complete_ready, this);
			port_set_busy_id(this, info[0], busy_id, res2);
			if (res == 0)
				res = res2;
		} else {
			complete_ready(info[0], this, res2, SPA_ID_INVALID);
		}
		changed = true;
	}

	free(this->info.format);
	this->info.format = format;

	if (changed)
		this->info.change_mask |= PW_LINK_CHANGE_MASK_FORMAT;

	pw_log_debug("%p: result %d", this, res);
	return res;

error:
	pw_context_debug_port_params(context, node[0],
			info[0]->port->direction, port_id[0],
			SPA_PARAM_EnumFormat, res, "input format (%s)", error);
	pw_context_debug_port_params(context, node[1],
			info[1]->port->direction, port_id[1],
			SPA_PARAM_EnumFormat, res, "output format (%s)", error);
	link_update_state(this, PW_LINK_STATE_ERROR, res, error);
	free(format);
	return res;
}

static int port_set_io(struct pw_impl_link *this, struct port_info *info, uint32_t id,
		void *data, size_t size)
{
	int res = 0;
	struct pw_impl_port *port = info->port;
	struct pw_impl_port_mix *mix = info->mix;

	pw_log_debug("%p: %s port %p %d.%d set io: %d %p %zd", this,
			pw_direction_as_string(port->direction),
			port, port->port_id, mix->port.port_id, id, data, size);

	if ((res = spa_node_port_set_io(port->mix,
			     mix->port.direction,
			     mix->port.port_id,
			     id, data, size)) < 0) {
		if (res == -ENOTSUP)
			res = 0;
		else
			pw_log_warn("%p: port %p can't set io:%d (%s): %s",
					this, port, id,
					spa_debug_type_find_name(spa_type_io, id),
					spa_strerror(res));
	}
	return res;
}

static void select_io(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct spa_io_buffers *io;

	io = this->rt.in_mix.io_data;
	if (io == NULL)
		io = this->rt.out_mix.io_data;
	if (io == NULL)
		io = impl->io;

	this->io = io;
	this->io[0] = SPA_IO_BUFFERS_INIT;
	this->io[1] = SPA_IO_BUFFERS_INIT;
}

static int do_allocation(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	uint32_t in_flags, out_flags, busy_id;
	char *error = NULL;
	struct pw_impl_port *input, *output;

	if (this->info.state > PW_LINK_STATE_ALLOCATING)
		return 0;

	output = this->output;
	input = this->input;

	pw_log_debug("%p: out-state:%d in-state:%d", this, output->state, input->state);

	if (input->state < PW_IMPL_PORT_STATE_READY || output->state < PW_IMPL_PORT_STATE_READY)
		return 0;

	link_update_state(this, PW_LINK_STATE_ALLOCATING, 0, NULL);

	out_flags = output->spa_flags;
	in_flags = input->spa_flags;

	pw_log_debug("%p: out-node:%p in-node:%p: out-flags:%08x in-flags:%08x",
			this, output->node, input->node, out_flags, in_flags);

	this->rt.in_mix.have_buffers = false;
	this->rt.out_mix.have_buffers = false;

	if (out_flags & SPA_PORT_FLAG_LIVE) {
		pw_log_debug("%p: setting link as live", this);
		output->node->live = true;
		input->node->live = true;
	}

	if (output->buffers.n_buffers) {
		pw_log_debug("%p: reusing %d output buffers %p", this,
				output->buffers.n_buffers, output->buffers.buffers);
		this->rt.out_mix.have_buffers = true;
	} else {
		uint32_t flags, alloc_flags;
		struct spa_node *in_node, *out_node;
		uint32_t in_port, out_port;

		flags = 0;
		/* always enable async mode */
		alloc_flags = PW_BUFFERS_FLAG_ASYNC;

		if (output->node->remote || input->node->remote)
			alloc_flags |= PW_BUFFERS_FLAG_SHARED;

		if (output->node->driver)
			alloc_flags |= PW_BUFFERS_FLAG_IN_PRIORITY;

		/* if output port can alloc buffers, alloc skeleton buffers */
		if (SPA_FLAG_IS_SET(out_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS)) {
			SPA_FLAG_SET(alloc_flags, PW_BUFFERS_FLAG_NO_MEM);
			flags |= SPA_NODE_BUFFERS_FLAG_ALLOC;
		}
#if 0
		in_node = input->mix;
		in_port = this->rt.in_mix.port.port_id;
		out_node = output->mix;
		out_port = this->rt.out_mix.port.port_id;
#else
		in_node = input->node->node;
		in_port = input->port_id;
		out_node = output->node->node;
		out_port = output->port_id;
#endif

		if ((res = pw_buffers_negotiate(this->context, alloc_flags,
						out_node, out_port,
						in_node, in_port,
						&output->buffers)) < 0) {
			error = spa_aprintf("error alloc buffers: %s", spa_strerror(res));
			goto error;
		}

		pw_log_debug("%p: allocating %d buffers %p", this,
			     output->buffers.n_buffers, output->buffers.buffers);

		if ((res = pw_impl_port_use_buffers(output, &this->rt.out_mix, flags,
						output->buffers.buffers,
						output->buffers.n_buffers)) < 0) {
			error = spa_aprintf("error use output buffers: %d (%s)", res,
					spa_strerror(res));
			goto error_clear;
		}
		if (SPA_RESULT_IS_ASYNC(res)) {
			busy_id = pw_work_queue_add(impl->work, &impl->output,
					spa_node_sync(output->node->node, res),
					complete_paused, this);
			port_set_busy_id(this, &impl->output, busy_id, res);
			if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC)
				return 0;
		} else {
			complete_paused(&impl->output, this, res, SPA_ID_INVALID);
		}
	}

	pw_log_debug("%p: using %d buffers %p on input port", this,
		     output->buffers.n_buffers, output->buffers.buffers);

	if ((res = pw_impl_port_use_buffers(input, &this->rt.in_mix, 0,
				output->buffers.buffers,
				output->buffers.n_buffers)) < 0) {
		error = spa_aprintf("error use input buffers: %d (%s)", res,
				spa_strerror(res));
		goto error;
	}

	if (SPA_RESULT_IS_ASYNC(res)) {
		busy_id = pw_work_queue_add(impl->work, &impl->input,
				spa_node_sync(input->node->node, res),
				complete_paused, this);
		port_set_busy_id(this, &impl->input, busy_id, res);
	} else {
		complete_paused(&impl->input, this, res, SPA_ID_INVALID);
	}
	return 0;

error_clear:
	pw_buffers_clear(&output->buffers);
error:
	link_update_state(this, PW_LINK_STATE_ERROR, res, error);
	return res;
}

int pw_impl_link_activate(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	uint32_t io_type, io_size;
	bool reliable_driver;

	pw_log_debug("%p: activate activated:%d state:%s", this, impl->activated,
			pw_link_state_as_string(this->info.state));

	if (this->destroyed || impl->activated || !this->prepared ||
		!impl->input.node->runnable || !impl->output.node->runnable)
		return 0;

	/* check if the output node is a driver for the input node and if
	 * it has reliable scheduling. Because it is a driver, it will always be
	 * scheduled before the input node and there will not be any concurrent access
	 * to the io, so we don't need async IO, even when the input is async. This
	 * avoid the problem of out-of-order buffers after a stall. */
	reliable_driver = (impl->output.node == impl->input.node->driver_node) &&
		impl->output.node->reliable;

	if (this->async && !reliable_driver) {
		io_type = SPA_IO_AsyncBuffers;
		io_size = sizeof(struct spa_io_async_buffers);
	} else {
		io_type = SPA_IO_Buffers;
		io_size = sizeof(struct spa_io_buffers);
	}

	if ((res = port_set_io(this, &impl->input, io_type, this->io, io_size)) < 0)
		goto error;
	if ((res = port_set_io(this, &impl->output, io_type, this->io, io_size)) < 0)
		goto error_clean;

	impl->activated = true;
	pw_log_info("(%s) activated", this->name);
	link_update_state(this, PW_LINK_STATE_ACTIVE, 0, NULL);

	return 0;

error_clean:
	port_set_io(this, &impl->input, io_type, NULL, 0);
error:
	pw_log_error("%p: can't activate link: %s", this, spa_strerror(res));
	return res;
}

static void check_states(void *obj, void *user_data, int res, uint32_t id)
{
	struct pw_impl_link *this = obj;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int in_state, out_state;
	struct pw_impl_port *input, *output;

	if (this->info.state == PW_LINK_STATE_ERROR)
		return;

	if (this->info.state >= PW_LINK_STATE_PAUSED)
		return;

	output = this->output;
	input = this->input;

	if (output == NULL || input == NULL) {
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO,
				strdup("link without input or output port"));
		return;
	}

	if (output->node->info.state == PW_NODE_STATE_ERROR ||
	    input->node->info.state == PW_NODE_STATE_ERROR) {
		pw_log_warn("%p: one of the nodes is in error out:%s in:%s", this,
				pw_node_state_as_string(output->node->info.state),
				pw_node_state_as_string(input->node->info.state));
		return;
	}

	out_state = output->state;
	in_state = input->state;

	pw_log_debug("%p: output state %d, input state %d", this, out_state, in_state);

	if (out_state == PW_IMPL_PORT_STATE_ERROR || in_state == PW_IMPL_PORT_STATE_ERROR) {
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO, strdup("ports are in error"));
		return;
	}

	if (PW_IMPL_PORT_IS_CONTROL(output) && PW_IMPL_PORT_IS_CONTROL(input)) {
		pw_impl_port_update_state(output, PW_IMPL_PORT_STATE_PAUSED, 0, NULL);
		pw_impl_port_update_state(input, PW_IMPL_PORT_STATE_PAUSED, 0, NULL);
		link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
	}

	if (output->busy_count > 0) {
		pw_log_debug("%p: output port %p was busy %d", this, output, output->busy_count);
		res = spa_node_sync(output->node->node, 0);
		pw_work_queue_add(impl->work, &impl->output, res, complete_sync, this);
		goto exit;
	}
	else if (input->busy_count > 0) {
		pw_log_debug("%p: input port %p was busy %d", this, input, input->busy_count);
		res = spa_node_sync(input->node->node, 0);
		pw_work_queue_add(impl->work, &impl->input, res, complete_sync, this);
		goto exit;
	}

	if ((res = do_negotiate(this)) != 0)
		goto exit;

	if ((res = do_allocation(this)) != 0)
		goto exit;

exit:
	if (SPA_RESULT_IS_ERROR(res)) {
		pw_log_debug("%p: got error result %d (%s)", this, res, spa_strerror(res));
		return;
	}

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);
}

static void input_remove(struct pw_impl_link *this)
{
	struct impl *impl = (struct impl *) this;
	struct port_info *info = &impl->input;
	struct pw_impl_port_mix *mix = info->mix;
	struct pw_impl_port *port = info->port;
	int res;

	pw_log_debug("%p: remove input port %p", this, port);

	port_set_busy_id(this, info, SPA_ID_INVALID, SPA_ID_INVALID);

	spa_hook_remove(&info->port_listener);
	spa_hook_remove(&info->node_listener);
	spa_hook_remove(&info->global_listener);

	spa_list_remove(&this->input_link);
	pw_impl_port_emit_link_removed(port, this);

	pw_impl_port_recalc_capability(port);
	pw_impl_port_recalc_latency(port);
	pw_impl_port_recalc_tag(port);

	if ((res = port_set_io(this, info, SPA_IO_Buffers, NULL, 0)) < 0)
		pw_log_warn("%p: port %p set_io error %s", this, port, spa_strerror(res));
	if ((res = pw_impl_port_use_buffers(port, mix, 0, NULL, 0)) < 0)
		pw_log_warn("%p: port %p clear error %s", this, port, spa_strerror(res));

	pw_impl_port_release_mix(port, mix);

	pw_work_queue_cancel(impl->work, info, SPA_ID_INVALID);
	this->input = NULL;
}

static void output_remove(struct pw_impl_link *this)
{
	struct impl *impl = (struct impl *) this;
	struct port_info *info = &impl->output;
	struct pw_impl_port_mix *mix = info->mix;
	struct pw_impl_port *port = info->port;

	pw_log_debug("%p: remove output port %p", this, port);

	port_set_busy_id(this, info, SPA_ID_INVALID, SPA_ID_INVALID);

	spa_hook_remove(&info->port_listener);
	spa_hook_remove(&info->node_listener);
	spa_hook_remove(&info->global_listener);

	spa_list_remove(&this->output_link);
	pw_impl_port_emit_link_removed(port, this);

	pw_impl_port_recalc_capability(port);
	pw_impl_port_recalc_latency(port);
	pw_impl_port_recalc_tag(port);

	/* we don't clear output buffers when the link goes away. They will get
	 * cleared when the node goes to suspend */
	pw_impl_port_release_mix(port, mix);

	pw_work_queue_cancel(impl->work, info, SPA_ID_INVALID);
	this->output = NULL;
}

SPA_EXPORT
int pw_impl_link_prepare(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("%p: prepared:%d preparing:%d in_active:%d out_active:%d passive:%u",
			this, this->prepared, this->preparing,
			impl->input.node->active, impl->output.node->active, this->passive);

	if (!impl->input.node->active || !impl->output.node->active)
		return 0;

	if (this->destroyed || this->preparing || this->prepared)
		return 0;

	this->preparing = true;

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);

	return 0;
}

int pw_impl_link_deactivate(struct pw_impl_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("%p: deactivate activated:%d", this, impl->activated);

	if (!impl->activated)
		return 0;

	port_set_io(this, &impl->output, SPA_IO_Buffers, NULL, 0);
	port_set_io(this, &impl->input, SPA_IO_Buffers, NULL, 0);

	impl->activated = false;
	pw_log_info("(%s) deactivated", this->name);

	if (this->info.state < PW_LINK_STATE_PAUSED || this->destroyed)
		link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
	else
		link_update_state(this, PW_LINK_STATE_PAUSED, 0, NULL);
	return 0;
}

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_impl_link *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, permissions, global->type, version, 0);
	if (resource == NULL)
		goto error_resource;

	pw_log_debug("%p: bound to %d", this, resource->id);
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_LINK_CHANGE_MASK_ALL;
	pw_link_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error("%p: can't create link resource: %m", this);
	return -errno;
}

static void port_state_changed(struct pw_impl_link *this, struct pw_impl_port *port,
		struct pw_impl_port *other, enum pw_impl_port_state old,
		enum pw_impl_port_state state, const char *error)
{
	pw_log_debug("%p: port %p old:%d -> state:%d prepared:%d preparing:%d",
			this, port, old, state, this->prepared, this->preparing);

	switch (state) {
	case PW_IMPL_PORT_STATE_ERROR:
		link_update_state(this, PW_LINK_STATE_ERROR, -EIO, error ? strdup(error) : NULL);
		break;
	case PW_IMPL_PORT_STATE_INIT:
	case PW_IMPL_PORT_STATE_CONFIGURE:
		if (this->prepared || state < old) {
			this->prepared = false;
			link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
		}
		break;
	case PW_IMPL_PORT_STATE_READY:
		if (this->prepared || state < old) {
			this->prepared = false;
			link_update_state(this, PW_LINK_STATE_NEGOTIATING, 0, NULL);
		}
		break;
	case PW_IMPL_PORT_STATE_PAUSED:
		break;
	}
}

static void port_param_changed(struct pw_impl_link *this, uint32_t id,
		struct pw_impl_port *outport, struct pw_impl_port *inport)
{
	enum pw_impl_port_state target;

	pw_log_debug("%p: outport %p input %p param %d (%s)", this,
		outport, inport, id, spa_debug_type_find_name(spa_type_param, id));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		target = PW_IMPL_PORT_STATE_CONFIGURE;
		break;
	default:
		return;
	}
	if (outport)
		pw_impl_port_update_state(outport, target, 0, NULL);
	if (inport)
		pw_impl_port_update_state(inport, target, 0, NULL);

	pw_log_info("%p: format changed", this);
	this->preparing = this->prepared = false;
	link_update_state(this, PW_LINK_STATE_INIT, 0, NULL);
	pw_impl_link_prepare(this);
}

static void input_port_param_changed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_param_changed(this, id, this->output, this->input);
}

static void input_port_state_changed(void *data, enum pw_impl_port_state old,
			enum pw_impl_port_state state, const char *error)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_state_changed(this, this->input, this->output, old, state, error);
}

static void output_port_param_changed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_param_changed(this, id, this->output, this->input);
}

static void output_port_state_changed(void *data, enum pw_impl_port_state old,
			enum pw_impl_port_state state, const char *error)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	port_state_changed(this, this->output, this->input, old, state, error);
}

static void input_port_latency_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_latency(this->output);
}

static void input_port_tag_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_tag(this->output);
}

static void input_port_capability_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_capability(this->output);
}

static void output_port_latency_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_latency(this->input);
}

static void output_port_tag_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_tag(this->input);
}

static void output_port_capability_changed(void *data)
{
	struct impl *impl = data;
	struct pw_impl_link *this = &impl->this;
	if (!this->feedback)
		pw_impl_port_recalc_capability(this->input);
}

static const struct pw_impl_port_events input_port_events = {
	PW_VERSION_IMPL_PORT_EVENTS,
	.param_changed = input_port_param_changed,
	.state_changed = input_port_state_changed,
	.latency_changed = input_port_latency_changed,
	.tag_changed = input_port_tag_changed,
	.capability_changed = input_port_capability_changed,
};

static const struct pw_impl_port_events output_port_events = {
	PW_VERSION_IMPL_PORT_EVENTS,
	.param_changed = output_port_param_changed,
	.state_changed = output_port_state_changed,
	.latency_changed = output_port_latency_changed,
	.tag_changed = output_port_tag_changed,
	.capability_changed = output_port_capability_changed,
};

static void node_result(struct impl *impl, void *obj,
		int seq, int res, uint32_t type, const void *result)
{
	struct port_info *info = obj;
	struct pw_impl_port *port = info->port;

	pw_log_trace("%p: %s port %p result seq:%d %d res:%d type:%u",
			impl, pw_direction_as_string(port->direction),
			port, seq, SPA_RESULT_ASYNC_SEQ(seq), res, type);

	if (type == SPA_RESULT_TYPE_NODE_ERROR && info->pending_seq == seq)
		info->result = res;

	if (SPA_RESULT_IS_ASYNC(seq))
		pw_work_queue_complete(impl->work, obj, SPA_RESULT_ASYNC_SEQ(seq), res);
}

static void input_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	node_result(impl, &impl->input, seq, res, type, result);
}

static void output_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	node_result(impl, &impl->output, seq, res, type, result);
}

static void node_active_changed(void *data, bool active)
{
	struct impl *impl = data;
	pw_impl_link_prepare(&impl->this);
}

static void node_driver_changed(void *data, struct pw_impl_node *old, struct pw_impl_node *driver)
{
	struct impl *impl = data;
	if (impl->this.async) {
		/* for async links, input and output port latency depends on if the
		 * output node is directly driving the input node. */
		pw_impl_port_recalc_latency(impl->output.port);
		pw_impl_port_recalc_latency(impl->input.port);
	}
}

static const struct pw_impl_node_events input_node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.result = input_node_result,
	.active_changed = node_active_changed,
	.driver_changed = node_driver_changed,
};

static const struct pw_impl_node_events output_node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.result = output_node_result,
	.active_changed = node_active_changed,
	.driver_changed = node_driver_changed,
};

static bool pw_impl_node_can_reach(struct pw_impl_node *output, struct pw_impl_node *input, int hop)
{
	struct pw_impl_port *p;
	struct pw_impl_link *l;

	output->loopchecked = true;

	if (output == input)
		return true;

	if (hop == MAX_HOPS) {
		pw_log_warn("exceeded hops (%d) %s -> %s", hop, output->name, input->name);
		return false;
	}

	spa_list_for_each(p, &output->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link)
			l->input->node->loopchecked = l->feedback;
	}
	spa_list_for_each(p, &output->output_ports, link) {
		spa_list_for_each(l, &p->links, output_link) {
			if (l->input->node->loopchecked)
				continue;
			if (pw_impl_node_can_reach(l->input->node, input, hop+1))
				return true;
		}
	}
	return false;
}

static void try_link_controls(struct impl *impl, struct pw_impl_port *output, struct pw_impl_port *input)
{
	struct pw_control *cin, *cout;
	struct pw_impl_link *this = &impl->this;
	uint32_t omix, imix;
	int res;

	imix = this->rt.in_mix.port.port_id;
	omix = this->rt.out_mix.port.port_id;

	pw_log_debug("%p: trying controls", impl);
	spa_list_for_each(cout, &output->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &input->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_add_link(cout, omix, cin, imix, &this->control)) < 0)
				pw_log_error("%p: failed to link controls: %s",
						this, spa_strerror(res));
			break;
		}
	}
	spa_list_for_each(cin, &output->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &input->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_add_link(cout, imix, cin, omix, &this->notify)) < 0)
				pw_log_error("%p: failed to link controls: %s",
						this, spa_strerror(res));
			break;
		}
	}
}

static void try_unlink_controls(struct impl *impl, struct pw_impl_port *output, struct pw_impl_port *input)
{
	struct pw_impl_link *this = &impl->this;
	int res;

	pw_log_debug("%p: unlinking controls", impl);
	if (this->control.valid) {
		if ((res = pw_control_remove_link(&this->control)) < 0)
			pw_log_error("%p: failed to unlink controls: %s",
					this, spa_strerror(res));
	}
	if (this->notify.valid) {
		if ((res = pw_control_remove_link(&this->notify)) < 0)
			pw_log_error("%p: failed to unlink controls: %s",
					this, spa_strerror(res));
	}
}

static int check_owner_permissions(struct pw_context *context,
		struct pw_impl_node *node, struct pw_global *other, uint32_t permissions)
{
	const char *str;
	struct pw_impl_client *client;
	struct pw_global *global;
	uint32_t perms;
	uint32_t client_id;

	str = pw_properties_get(node->properties, PW_KEY_CLIENT_ID);
	if (str == NULL)
		/* node not owned by client */
		return 0;

	if (!spa_atou32(str, &client_id, 0))
		/* invalid client_id, something is wrong */
		return -EIO;
	if ((global = pw_context_find_global(context, client_id)) == NULL)
		/* current client can't see the owner client */
		return -errno;
	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Client) ||
	    (client = global->object) == NULL)
		/* not the right object, something wrong */
		return -EIO;

	perms = pw_global_get_permissions(other, client);
	if ((perms & permissions) != permissions)
		/* owner client can't see other node */
		return -EPERM;

	return 0;
}

static int
check_permission(struct pw_context *context,
		 struct pw_impl_port *output,
		 struct pw_impl_port *input,
		 struct pw_properties *properties)
{
	int res;
	uint32_t in_perms, out_perms;
	struct pw_global *in_global, *out_global;

	if ((in_global = input->node->global) == NULL)
		return -ENOENT;
	if ((out_global = output->node->global) == NULL)
		return -ENOENT;

	in_perms = out_perms = PW_PERM_R | PW_PERM_L;
	if (context->current_client != NULL) {
		in_perms = pw_global_get_permissions(in_global, context->current_client);
		out_perms = pw_global_get_permissions(out_global, context->current_client);
	}
	/* current client can't see input node or output node */
	if (!PW_PERM_IS_R(in_perms) || !PW_PERM_IS_R(out_perms))
		return -ENOENT;

	if ((res = check_owner_permissions(context, output->node,
					in_global, PW_PERM_R)) < 0) {
		/* output node owner can't see input node, check if the current
		 * client has universal link permissions for the output node */
		if (!PW_PERM_IS_L(out_perms))
			return res;
	}
	if ((res = check_owner_permissions(context, input->node,
					out_global, PW_PERM_R)) < 0) {
		/* input node owner can't see output node, check if the current
		 * client has universal link permissions for the input node */
		if (!PW_PERM_IS_L(in_perms))
			return res;
	}
	return 0;
}

static void permissions_changed(struct pw_impl_link *this, struct pw_impl_port *other,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	int res;
	uint32_t perm;

	perm = pw_global_get_permissions(other->global, client);
	old &= perm;
	new &= perm;
	pw_log_debug("%p: permissions changed %08x -> %08x", this, old, new);

	if ((res = check_permission(this->context, this->output, this->input, this->properties)) < 0) {
		pw_log_info("%p: link permissions removed: %s", this, spa_strerror(res));
		pw_impl_link_destroy(this);
	} else if (this->global != NULL) {
		pw_global_update_permissions(this->global, client, old, new);
	}
}

static bool is_port_owner(struct pw_impl_client *client, struct pw_impl_port *port)
{
	const char *str;
	uint32_t client_id;

	str = pw_properties_get(port->node->properties, PW_KEY_CLIENT_ID);
	if (str == NULL)
		return false;

	if (!spa_atou32(str, &client_id, 0))
		return false;

	return client_id == client->info.id;
}

static void output_permissions_changed(void *data,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	struct pw_impl_link *this = data;
	if (!is_port_owner(client, this->output) &&
	    !is_port_owner(client, this->input))
		return;
	permissions_changed(this, this->input, client, old, new);
}

static const struct pw_global_events output_global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.permissions_changed = output_permissions_changed,
};

static void input_permissions_changed(void *data,
		struct pw_impl_client *client, uint32_t old, uint32_t new)
{
	struct pw_impl_link *this = data;
	if (!is_port_owner(client, this->output) &&
	    !is_port_owner(client, this->input))
		return;
	permissions_changed(this, this->output, client, old, new);
}

static const struct pw_global_events input_global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.permissions_changed = input_permissions_changed,
};

SPA_EXPORT
struct pw_impl_link *pw_context_create_link(struct pw_context *context,
			    struct pw_impl_port *output,
			    struct pw_impl_port *input,
			    struct spa_pod *format_filter,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_impl_link *this;
	struct pw_impl_node *input_node, *output_node;
	const char *str;
	int res;

	if (output == input)
		goto error_same_ports;

	if (output->direction != PW_DIRECTION_OUTPUT ||
	    input->direction != PW_DIRECTION_INPUT)
		goto error_wrong_direction;

	if (pw_impl_link_find(output, input))
		goto error_link_exists;

	if (check_permission(context, output, input, properties) < 0)
		goto error_link_not_allowed;

	output_node = output->node;
	input_node = input->node;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto error_no_mem;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto error_no_mem;

	impl->input.busy_id = SPA_ID_INVALID;
	impl->output.busy_id = SPA_ID_INVALID;

	this = &impl->this;
	this->feedback = pw_impl_node_can_reach(input_node, output_node, 0);
	pw_properties_set(properties, PW_KEY_LINK_FEEDBACK, this->feedback ? "true" : NULL);

	pw_log_debug("%p: new out-port:%p -> in-port:%p", this, output, input);

	if (user_data_size > 0)
                this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	impl->work = pw_context_get_work_queue(context);

	this->context = context;
	this->properties = properties;
	this->info.props = &this->properties->dict;
	this->info.state = PW_LINK_STATE_INIT;

	this->output = output;
	this->input = input;

	/* passive means that this link does not make the nodes active */
	str = pw_properties_get(properties, PW_KEY_LINK_PASSIVE);
	this->passive = str ? spa_atob(str) :
		(output->passive && input_node->can_suspend) ||
		(input->passive && output_node->can_suspend) ||
		(input->passive && output->passive);
	if (this->passive && str == NULL)
		 pw_properties_set(properties, PW_KEY_LINK_PASSIVE, "true");

	this->async = (output_node->async || input_node->async) &&
		SPA_FLAG_IS_SET(output->flags, PW_IMPL_PORT_FLAG_ASYNC) &&
		SPA_FLAG_IS_SET(input->flags, PW_IMPL_PORT_FLAG_ASYNC);

	if (this->async)
		 pw_properties_set(properties, PW_KEY_LINK_ASYNC, "true");

	spa_hook_list_init(&this->listener_list);

	impl->format_filter = format_filter;
	this->info.format = NULL;

	this->rt.out_mix.peer_id = input->global->id;
	this->rt.in_mix.peer_id = output->global->id;

	if ((res = pw_impl_port_init_mix(output, &this->rt.out_mix)) < 0)
		goto error_output_mix;
	if ((res = pw_impl_port_init_mix(input, &this->rt.in_mix)) < 0)
		goto error_input_mix;

	pw_impl_port_add_listener(input, &impl->input.port_listener, &input_port_events, impl);
	pw_impl_node_add_listener(input_node, &impl->input.node_listener, &input_node_events, impl);
	pw_global_add_listener(input->global, &impl->input.global_listener, &input_global_events, impl);
	pw_impl_port_add_listener(output, &impl->output.port_listener, &output_port_events, impl);
	pw_impl_node_add_listener(output_node, &impl->output.node_listener, &output_node_events, impl);
	pw_global_add_listener(output->global, &impl->output.global_listener, &output_global_events, impl);

	input_node->live = output_node->live;

	pw_log_debug("%p: output node %p live %d, feedback %d",
			this, output_node, output_node->live, this->feedback);

	spa_list_append(&output->links, &this->output_link);
	spa_list_append(&input->links, &this->input_link);

	select_io(this);

	impl->input.port = input;
	impl->output.port = output;
	impl->output.node = output_node;
	impl->input.node = input_node;
	impl->input.mix = &this->rt.in_mix;
	impl->output.mix = &this->rt.out_mix;

	pw_log_debug("%p: constructed out:%p:%d.%d -> in:%p:%d.%d", impl,
		     output_node, output->port_id, this->rt.out_mix.port.port_id,
		     input_node, input->port_id, this->rt.in_mix.port.port_id);

	this->name = spa_aprintf("%d.%d.%d -> %d.%d.%d",
			output_node->info.id, output->port_id, this->rt.out_mix.port.port_id,
			input_node->info.id, input->port_id, this->rt.in_mix.port.port_id);
	pw_log_info("(%s) (%s) -> (%s) async:%d:%d:%d:%04x:%04x:%d", this->name, output_node->name,
			input_node->name, output_node->driving,
			output_node->async, input_node->async,
			output->flags, input->flags, this->async);

	pw_impl_port_emit_link_added(output, this);
	pw_impl_port_emit_link_added(input, this);

	try_link_controls(impl, output, input);

	pw_impl_port_recalc_capability(output);
	pw_impl_port_recalc_capability(input);
	pw_impl_port_recalc_latency(output);
	pw_impl_port_recalc_latency(input);
	pw_impl_port_recalc_tag(output);
	pw_impl_port_recalc_tag(input);

	if (impl->output.node != impl->input.node) {
		if (this->feedback)
			this->peer = pw_node_peer_ref(impl->input.node, impl->output.node);
		else
			this->peer = pw_node_peer_ref(impl->output.node, impl->input.node);
	}

	return this;

error_same_ports:
	res = -EINVAL;
	pw_log_debug("can't link the same ports");
	goto error_exit;
error_wrong_direction:
	res = -EINVAL;
	pw_log_debug("ports have wrong direction");
	goto error_exit;
error_link_exists:
	res = -EEXIST;
	pw_log_debug("link already exists");
	goto error_exit;
error_link_not_allowed:
	res = -EPERM;
	pw_log_debug("link not allowed");
	goto error_exit;
error_no_mem:
	res = -errno;
	pw_log_debug("alloc failed: %m");
	goto error_exit;
error_output_mix:
	pw_log_error("%p: can't get output mix %d (%s)", this, res, spa_strerror(res));
	goto error_free;
error_input_mix:
	pw_log_error("%p: can't get input mix %d (%s)", this, res, spa_strerror(res));
	pw_impl_port_release_mix(output, &this->rt.out_mix);
	goto error_free;
error_free:
	free(impl);
error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static void global_destroy(void *data)
{
	struct pw_impl_link *link = data;
	spa_hook_remove(&link->global_listener);
	link->global = NULL;
	pw_impl_link_destroy(link);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_impl_link_register(struct pw_impl_link *link,
		     struct pw_properties *properties)
{
	static const char * const keys[] = {
		PW_KEY_OBJECT_PATH,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		PW_KEY_LINK_OUTPUT_PORT,
		PW_KEY_LINK_INPUT_PORT,
		PW_KEY_LINK_OUTPUT_NODE,
		PW_KEY_LINK_INPUT_NODE,
		NULL
	};

	struct pw_context *context = link->context;
	struct pw_impl_node *output_node, *input_node;

	if (link->registered)
		goto error_existed;

	output_node = link->output->node;
	input_node = link->input->node;

	link->info.output_node_id = output_node->global->id;
	link->info.output_port_id = link->output->global->id;
	link->info.input_node_id = input_node->global->id;
	link->info.input_port_id = link->input->global->id;

	link->global = pw_global_new(context,
				     PW_TYPE_INTERFACE_Link,
				     PW_VERSION_LINK,
				     PW_LINK_PERM_MASK,
				     properties,
				     global_bind,
				     link);
	if (link->global == NULL)
		return -errno;

	spa_list_append(&context->link_list, &link->link);
	link->registered = true;

	link->info.id = link->global->id;
	pw_properties_setf(link->properties, PW_KEY_OBJECT_ID, "%d", link->info.id);
	pw_properties_setf(link->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(link->global));
	pw_properties_setf(link->properties, PW_KEY_LINK_OUTPUT_NODE, "%u", link->info.output_node_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_OUTPUT_PORT, "%u", link->info.output_port_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_INPUT_NODE, "%u", link->info.input_node_id);
	pw_properties_setf(link->properties, PW_KEY_LINK_INPUT_PORT, "%u", link->info.input_port_id);

	pw_global_update_keys(link->global, link->info.props, keys);

	pw_impl_link_emit_initialized(link);

	pw_global_add_listener(link->global, &link->global_listener, &global_events, link);
	pw_global_register(link->global);

	pw_impl_link_prepare(link);

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

SPA_EXPORT
void pw_impl_link_destroy(struct pw_impl_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	bool was_prepared = link->prepared;

	pw_log_debug("%p: destroy", impl);
	pw_log_info("(%s) destroy", link->name);

	link->destroyed = true;
	pw_impl_link_emit_destroy(link);

	pw_impl_link_deactivate(link);

	if (link->registered)
		spa_list_remove(&link->link);

	if (link->peer)
		pw_node_peer_unref(link->peer);

	try_unlink_controls(impl, link->output, link->input);

	output_remove(link);
	input_remove(link);

	if (link->global) {
		spa_hook_remove(&link->global_listener);
		pw_global_destroy(link->global);
	}

	if (was_prepared)
		pw_context_recalc_graph(link->context, "link destroy");

	pw_log_debug("%p: free", impl);
	pw_impl_link_emit_free(link);

	pw_work_queue_cancel(impl->work, link, SPA_ID_INVALID);

	spa_hook_list_clean(&link->listener_list);

	pw_properties_free(link->properties);

	free(link->name);
	free(link->info.format);
	free((char *) link->info.error);
	free(impl);
}

SPA_EXPORT
void pw_impl_link_add_listener(struct pw_impl_link *link,
			  struct spa_hook *listener,
			  const struct pw_impl_link_events *events,
			  void *data)
{
	pw_log_debug("%p: add listener %p", link, listener);
	spa_hook_list_append(&link->listener_list, listener, events, data);
}

struct pw_impl_link *pw_impl_link_find(struct pw_impl_port *output_port, struct pw_impl_port *input_port)
{
	struct pw_impl_link *pl;

	spa_list_for_each(pl, &output_port->links, output_link) {
		if (pl->input == input_port)
			return pl;
	}
	return NULL;
}

SPA_EXPORT
struct pw_context *pw_impl_link_get_context(struct pw_impl_link *link)
{
	return link->context;
}

SPA_EXPORT
void *pw_impl_link_get_user_data(struct pw_impl_link *link)
{
	return link->user_data;
}

SPA_EXPORT
const struct pw_link_info *pw_impl_link_get_info(struct pw_impl_link *link)
{
	return &link->info;
}

SPA_EXPORT
struct pw_global *pw_impl_link_get_global(struct pw_impl_link *link)
{
	return link->global;
}

SPA_EXPORT
struct pw_impl_port *pw_impl_link_get_output(struct pw_impl_link *link)
{
	return link->output;
}

SPA_EXPORT
struct pw_impl_port *pw_impl_link_get_input(struct pw_impl_link *link)
{
	return link->input;
}
