/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/list.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/pod/filter.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.control-mixer");

#define MAX_BUFFERS     64
#define MAX_PORTS       512

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1 << 0)
	uint32_t flags;

	struct spa_list link;
	struct spa_buffer *buffer;
};

struct port {
	struct spa_list link;

	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io[2];

	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[8];

	unsigned int have_format:1;

	uint32_t types;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;

	struct spa_list mix_link;
	bool active;
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	struct spa_pod_control control;
	const void *control_body;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	uint32_t quantum_limit;

	struct spa_log *log;

	struct spa_loop *data_loop;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[8];

	struct spa_io_position *position;

	struct spa_hook_list hooks;

	struct port *in_ports[MAX_PORTS];
	struct port out_ports[1];
	struct spa_list port_list;
	struct spa_list free_list;

	int n_formats;

	unsigned int have_format:1;
	unsigned int started:1;

	struct spa_list mix_list;
	struct port *mix_ports[MAX_PORTS];
};

#define CHECK_ANY_IN(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) == SPA_ID_INVALID)
#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)] == NULL)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)] != NULL)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define CHECK_PORT_ANY(this,d,p)     (CHECK_ANY_IN(this,d,p) || CHECK_PORT(this,d,p))
#define GET_IN_PORT(this,p)          (this->in_ports[p])
#define GET_OUT_PORT(this,p)         (&this->out_ports[p])
#define GET_PORT(this,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))
#define GET_PORT_ANY(this,d,p)       (CHECK_ANY_IN(this,d,p) ? NULL : GET_PORT(this,d,p))

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	return -ENOTSUP;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	switch (id) {
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, GET_OUT_PORT(this, 0), true);
	spa_list_for_each(port, &this->port_list, link)
		emit_port_info(this, port, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

static struct port *get_free_port(struct impl *this)
{
	struct port *port;
	if (!spa_list_is_empty(&this->free_list)) {
		port = spa_list_first(&this->free_list, struct port, link);
		spa_list_remove(&port->link);
	} else {
		port = calloc(1, sizeof(struct port));
	}
	return port;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_FREE_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT (this, port_id);
	if ((port = get_free_port(this)) == NULL)
		return -errno;

	port->direction = direction;
	port->id = port_id;

	spa_list_init(&port->queue);
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
			   SPA_PORT_FLAG_DYNAMIC_DATA |
			   SPA_PORT_FLAG_REMOVABLE |
			   SPA_PORT_FLAG_OPTIONAL;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;

	this->in_ports[port_id] = port;
	spa_list_append(&this->port_list, &port->link);

	spa_log_debug(this->log, "%p: add port %d:%d", this, direction, port_id);
	emit_port_info(this, port, true);

	return 0;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT (this, port_id);
	this->in_ports[port_id] = NULL;
	spa_list_remove(&port->link);

	if (port->have_format && this->have_format) {
		if (--this->n_formats == 0)
			this->have_format = false;
	}
	spa_memzero(port, sizeof(struct port));
	spa_list_append(&this->free_list, &port->link);

	spa_log_debug(this->log, "%p: remove port %d:%d", this, direction, port_id);

	spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);

	return 0;
}

static int port_enum_formats(void *object, struct port *port,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	switch (index) {
	case 0:
		*param = spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT_ANY(this, direction, port_id), -EINVAL);

	port = GET_PORT_ANY(this, direction, port_id);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, port, result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (port == NULL || !port->have_format)
			return -EIO;
		if ((res = port_enum_formats(this, port, result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Buffers:
		if (port == NULL || !port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(this->quantum_limit,
				this->quantum_limit, INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(1));
		break;

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_AsyncBuffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_async_buffers)));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int queue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, "%p: queue buffer %d", this, b->id);
	return 0;
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace_fp(this->log, "%p: dequeue buffer %d", this, b->id);
	return b;
}

static int port_set_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = object;
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(!this->started || port->io == NULL, -EIO);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			if (--this->n_formats == 0)
				this->have_format = false;
			clear_buffers(this, port);
		}
	} else {
		uint32_t media_type, media_subtype, types = 0;
		if ((res = spa_format_parse(format, &media_type, &media_subtype)) < 0)
			return res;

		if (media_type != SPA_MEDIA_TYPE_application ||
		    media_subtype != SPA_MEDIA_SUBTYPE_control)
			return -EINVAL;

		if ((res = spa_pod_parse_object(format,
				SPA_TYPE_OBJECT_Format, NULL,
				SPA_FORMAT_CONTROL_types,  SPA_POD_OPT_Int(&types))) < 0)
			return res;

		this->have_format = true;

		if (!port->have_format) {
			this->n_formats++;
			port->have_format = true;
			port->types = types;
			spa_log_debug(this->log, "%p: set format on port %d:%d",
					this, direction, port_id);
		}
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(this, port, false);

	return 0;
}


static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format) {
		return port_set_format(this, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	spa_return_val_if_fail(!this->started || port->io == NULL, -EIO);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->buffer = buffers[i];
		b->flags = 0;
		b->id = i;

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: invalid memory on buffer %d", this, i);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, b);
	}
	port->n_buffers = n_buffers;

	return 0;
}

struct io_info {
	struct impl *impl;
	struct port *port;
	void *data;
	size_t size;
};

static int do_port_set_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct io_info *info = user_data;
	struct port *port = info->port;

	if (info->data == NULL || info->size < sizeof(struct spa_io_buffers)) {
		port->io[0] = NULL;
		port->io[1] = NULL;
		if (port->active) {
			spa_list_remove(&port->mix_link);
			port->active = false;
		}
	} else {
		if (info->size >= sizeof(struct spa_io_async_buffers)) {
			struct spa_io_async_buffers *ab = info->data;
			port->io[0] = &ab->buffers[port->direction];
			port->io[1] = &ab->buffers[port->direction^1];
		} else {
			port->io[0] = info->data;
			port->io[1] = info->data;
		}
		if (!port->active) {
			spa_list_append(&info->impl->mix_list, &port->mix_link);
			port->active = true;
		}
	}
	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;
	struct io_info info;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: port %d:%d io %d %p/%zd", this,
			direction, port_id, id, data, size);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	info.impl = this;
	info.port = port;
	info.data = data;
	info.size = size;

	switch (id) {
	case SPA_IO_Buffers:
	case SPA_IO_AsyncBuffers:
		spa_loop_locked(this->data_loop,
                               do_port_set_io, SPA_ID_INVALID, NULL, 0, &info);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);
	port = GET_OUT_PORT(this, 0);

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	return queue_buffer(this, port, &port->buffers[buffer_id]);
}

static inline int event_compare(uint8_t s1, uint8_t s2)
{
	/* 11 (controller) > 12 (program change) >
	 * 8 (note off) > 9 (note on) > 10 (aftertouch) >
	 * 13 (channel pressure) > 14 (pitch bend) */
	static int priotab[] = { 5,4,3,7,6,2,1,0 };
	if ((s1 & 0xf) != (s2 & 0xf))
		return 0;
	return priotab[(s2>>4) & 7] - priotab[(s1>>4) & 7];
}

static inline int event_sort(struct spa_pod_control *a, const void *abody,
		struct spa_pod_control *b, const void *bbody)
{
	if (a->offset < b->offset)
		return -1;
	if (a->offset > b->offset)
		return 1;
	if (a->type != b->type)
		return 0;
	switch(a->type) {
	case SPA_CONTROL_Midi:
	{
		const uint8_t *da = abody, *db = bbody;
		if (SPA_POD_BODY_SIZE(&a->value) < 1 || SPA_POD_BODY_SIZE(&b->value) < 1)
			return 0;
		return event_compare(da[0], db[0]);
	}
	case SPA_CONTROL_UMP:
	{
		const uint32_t *da = abody, *db = bbody;
		if (SPA_POD_BODY_SIZE(&a->value) < 4 || SPA_POD_BODY_SIZE(&b->value) < 4)
			return 0;
		if (((da[0] >> 28) != 2 && (da[0] >> 28) != 4) ||
		    ((db[0] >> 28) != 2 && (db[0] >> 28) != 4))
			return 0;
		return event_compare(da[0] >> 16, db[0] >> 16);
	}
	default:
		return 0;
	}
}

static inline bool control_needs_conversion(struct port *port, uint32_t type)
{
	/* we only converter between midi and UMP and only when the port
	 * does not support the current type */
	return (type == SPA_CONTROL_Midi || type == SPA_CONTROL_UMP) &&
	    port->types && (port->types & (1u << type)) == 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *outport, *inport;
	struct spa_io_buffers *outio;
	struct spa_pod_builder builder;
	uint32_t n_mix_ports;
	struct port **mix_ports;
	struct spa_pod_frame f;
	struct buffer *outb;
	struct spa_data *d;
	uint32_t cycle = this->position->clock.cycle & 1;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	outport = GET_OUT_PORT(this, 0);
	if ((outio = outport->io[cycle]) == NULL)
		return -EIO;

	spa_log_trace_fp(this->log, "%p: status %p %d %d",
			this, outio, outio->status, outio->buffer_id);

	if (outio->status == SPA_STATUS_HAVE_DATA)
		return outio->status;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		queue_buffer(this, outport, &outport->buffers[outio->buffer_id]);
		outio->buffer_id = SPA_ID_INVALID;
	}

	/* get output buffer */
	if ((outb = dequeue_buffer(this, outport)) == NULL) {
		if (outport->n_buffers > 0)
	                spa_log_warn(this->log, "%p: out of buffers (%d)",
					this, outport->n_buffers);
                return -EPIPE;
        }

	mix_ports = this->mix_ports;
	n_mix_ports = 0;

	/* collect all sequence pod on input ports */
	spa_list_for_each(inport, &this->mix_list, mix_link) {
		struct spa_io_buffers *inio = inport->io[cycle];
		struct spa_pod_sequence seq;
		const void *seq_body;

		if (inio->buffer_id >= inport->n_buffers ||
		    inio->status != SPA_STATUS_HAVE_DATA) {
			spa_log_trace_fp(this->log, "%p: skip input idx:%d "
					"io:%p status:%d buf_id:%d n_buffers:%d", this,
				inport->id, inio, inio->status, inio->buffer_id, inport->n_buffers);
			continue;
		}

		spa_log_trace_fp(this->log, "%p: mix input %d %p->%p %d %d", this,
				inport->id, inio, outio, inio->status, inio->buffer_id);

		d = inport->buffers[inio->buffer_id].buffer->datas;

		spa_pod_parser_init_from_data(&inport->parser, d->data, d->maxsize,
				d->chunk->offset, d->chunk->size);

		if (spa_pod_parser_push_sequence_body(&inport->parser,
					&inport->frame, &seq, &seq_body) < 0) {
			spa_log_trace_fp(this->log, "%p: skip input idx:%d max:%u "
					"offset:%u size:%u", this, inport->id,
					d->maxsize, d->chunk->offset, d->chunk->size);
			continue;
		}
		if (spa_pod_parser_get_control_body(&inport->parser,
					&inport->control, &inport->control_body) < 0) {
			spa_log_trace_fp(this->log, "%p: skip input idx:%d %u", this, inport->id, 
					inport->parser.state.offset);
			continue;
		}

		mix_ports[n_mix_ports++] = inport;
		inio->status = SPA_STATUS_NEED_DATA;
	}

	d = outb->buffer->datas;

	/* prepare to write into output */
	spa_pod_builder_init(&builder, d->data, d->maxsize);
	spa_pod_builder_push_sequence(&builder, &f, 0);

	/* merge sort all sequences into output buffer */
	while (true) {
		uint32_t i, next_index = 0;
		size_t size;
		uint8_t *body;
		struct port *next = NULL;
		struct spa_pod_control *control;

		for (i = 0; i < n_mix_ports; i++) {
			struct port *p = mix_ports[i];
			if (next == NULL || event_sort(&p->control, p->control_body,
						&next->control, next->control_body) <= 0) {
				next = p;
				next_index = i;
			}
		}
		if (next == NULL)
			break;

		control = &next->control;
		body = (uint8_t*)next->control_body;
		size = SPA_POD_BODY_SIZE(&control->value);

		if (control_needs_conversion(outport, control->type)) {
			switch (control->type) {
			case SPA_CONTROL_Midi:
			{
				uint32_t ump[4];
				uint64_t state = 0;
				while (size > 0) {
					int ump_size = spa_ump_from_midi(&body, &size, ump, sizeof(ump), 0, &state);
					if (ump_size <= 0)
						break;
					spa_pod_builder_control(&builder, control->offset, SPA_CONTROL_UMP);
					spa_pod_builder_bytes(&builder, ump, ump_size);
				}
				break;
			}
			case SPA_CONTROL_UMP:
			{
				uint8_t ev[8];
				const uint32_t *ump = (const uint32_t*)body;
				uint64_t state = 0;
				while (size > 0) {
					int ev_size = spa_ump_to_midi(&ump, &size, ev, sizeof(ev), &state);
					if (ev_size <= 0)
						break;
					spa_pod_builder_control(&builder, control->offset, SPA_CONTROL_Midi);
					spa_pod_builder_bytes(&builder, ev, ev_size);
				}
				break;
			}
			}
		} else {
			spa_pod_builder_control(&builder, control->offset, control->type);
			spa_pod_builder_primitive_body(&builder, &control->value, body, size, NULL, 0);
		}

		if (spa_pod_parser_get_control_body(&next->parser,
					&next->control, &next->control_body) < 0) {
			spa_pod_parser_pop(&next->parser, &next->frame);
			mix_ports[next_index] = mix_ports[--n_mix_ports];
		}
	}
	spa_pod_builder_pop(&builder, &f);

	if (builder.state.offset > d->maxsize) {
		spa_log_warn(this->log, "%p: control overflow %d > %d",
				this, builder.state.offset, d->maxsize);
		builder.state.offset = 0;
	}

	d->chunk->offset = 0;
	d->chunk->size = builder.state.offset;
	d->chunk->stride = 1;
	d->chunk->flags = 0;

	outio->buffer_id = outb->id;
	outio->status = SPA_STATUS_HAVE_DATA;

	return SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_list_insert_list(&this->free_list, &this->port_list);
	spa_list_consume(port, &this->free_list, link) {
		spa_list_remove(&port->link);
		free(port);
	}
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}

	this->quantum_limit = 8192;

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &this->quantum_limit, 0);
		}
	}

	spa_hook_list_init(&this->hooks);
	spa_list_init(&this->port_list);
	spa_list_init(&this->free_list);
	spa_list_init(&this->mix_list);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = 1;
	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	this->info.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_IN_DYNAMIC_PORTS;

	port = GET_OUT_PORT(this, 0);
	port->direction = SPA_DIRECTION_OUTPUT;
	port->id = 0;
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = SPA_PORT_FLAG_DYNAMIC_DATA;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 5;

	spa_list_init(&port->queue);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_control_mixer_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_CONTROL_MIXER,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
