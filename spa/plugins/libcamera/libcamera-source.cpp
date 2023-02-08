/* Spa libcamera source */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/*                         @author Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <deque>
#include <optional>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/ringbuffer.h>
#include <spa/monitor/device.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/control/control.h>
#include <spa/pod/filter.h>

#include <libcamera/camera.h>
#include <libcamera/stream.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer.h>
#include <libcamera/framebuffer_allocator.h>

#include "libcamera.h"
#include "libcamera-manager.hpp"

using namespace libcamera;

namespace {

#define MAX_BUFFERS	32
#define MASK_BUFFERS	31

#define BUFFER_FLAG_OUTSTANDING	(1<<0)
#define BUFFER_FLAG_ALLOCATED	(1<<1)
#define BUFFER_FLAG_MAPPED	(1<<2)

struct buffer {
	uint32_t id;
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	struct spa_meta_videotransform *videotransform;
	void *ptr;
};

#define MAX_CONTROLS	64

struct control {
	uint32_t id;
	uint32_t ctrl_id;
	double value;
};

struct port {
	struct impl *impl;

	std::optional<spa_video_info> current_format;

	struct spa_fraction rate = {};
	StreamConfiguration streamConfig;

	uint32_t memtype = 0;

	struct control controls[MAX_CONTROLS];
	uint32_t n_controls = 0;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers = 0;
	struct spa_list queue;
	struct spa_ringbuffer ring = SPA_RINGBUFFER_INIT();
	uint32_t ring_ids[MAX_BUFFERS];

	static constexpr uint64_t info_all = SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_PARAMS;
	struct spa_port_info info = SPA_PORT_INFO_INIT();
	struct spa_io_buffers *io = nullptr;
	struct spa_io_sequence *control = nullptr;
#define PORT_PropInfo	0
#define PORT_EnumFormat	1
#define PORT_Meta	2
#define PORT_IO		3
#define PORT_Format	4
#define PORT_Buffers	5
#define N_PORT_PARAMS	6
	struct spa_param_info params[N_PORT_PARAMS];

	uint32_t fmt_index = 0;
	PixelFormat enum_fmt;
	uint32_t size_index = 0;

	port(struct impl *impl)
		: impl(impl)
	{
		spa_list_init(&queue);

		params[PORT_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
		params[PORT_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
		params[PORT_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
		params[PORT_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
		params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);

		info.flags = SPA_PORT_FLAG_LIVE | SPA_PORT_FLAG_PHYSICAL | SPA_PORT_FLAG_TERMINAL;
		info.params = params;
		info.n_params = N_PORT_PARAMS;
	}
};

struct impl {
	struct spa_handle handle;
	struct spa_node node = {};

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *system;

	static constexpr uint64_t info_all =
		SPA_NODE_CHANGE_MASK_FLAGS |
		SPA_NODE_CHANGE_MASK_PROPS |
		SPA_NODE_CHANGE_MASK_PARAMS;
	struct spa_node_info info = SPA_NODE_INFO_INIT();
#define NODE_PropInfo	0
#define NODE_Props	1
#define NODE_EnumFormat	2
#define NODE_Format	3
#define N_NODE_PARAMS	4
	struct spa_param_info params[N_NODE_PARAMS];

	std::string device_id;
	std::string device_name;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks = {};

	std::array<port, 1> out_ports;

	struct spa_io_position *position = nullptr;
	struct spa_io_clock *clock = nullptr;

	std::shared_ptr<CameraManager> manager;
	std::shared_ptr<Camera> camera;

	FrameBufferAllocator *allocator = nullptr;
	std::vector<std::unique_ptr<libcamera::Request>> requestPool;
	std::deque<libcamera::Request *> pendingRequests;

	void requestComplete(libcamera::Request *request);

	std::unique_ptr<CameraConfiguration> config;

	struct spa_source source = {};

	ControlList ctrls;
	bool active = false;
	bool acquired = false;

	impl(spa_log *log, spa_loop *data_loop, spa_system *system,
	     std::shared_ptr<CameraManager> manager, std::shared_ptr<Camera> camera, std::string device_id);
};

}

#define CHECK_PORT(impl,direction,port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)

#define GET_OUT_PORT(impl,p)         (&impl->out_ports[p])
#define GET_PORT(impl,d,p)           GET_OUT_PORT(impl,p)

#include "libcamera-utils.cpp"

static int port_get_format(struct impl *impl, struct port *port,
			   uint32_t index,
			   const struct spa_pod *filter,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct spa_pod_frame f;

	if (!port->current_format)
		return -EIO;
	if (index > 0)
		return 0;

	spa_pod_builder_push_object(builder, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
	spa_pod_builder_add(builder,
		SPA_FORMAT_mediaType,    SPA_POD_Id(port->current_format->media_type),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(port->current_format->media_subtype),
		0);

	switch (port->current_format->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		spa_pod_builder_add(builder,
			SPA_FORMAT_VIDEO_format,    SPA_POD_Id(port->current_format->info.raw.format),
			SPA_FORMAT_VIDEO_size,      SPA_POD_Rectangle(&port->current_format->info.raw.size),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&port->current_format->info.raw.framerate),
			0);
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		spa_pod_builder_add(builder,
			SPA_FORMAT_VIDEO_size,      SPA_POD_Rectangle(&port->current_format->info.mjpg.size),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&port->current_format->info.mjpg.framerate),
			0);
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		spa_pod_builder_add(builder,
			SPA_FORMAT_VIDEO_size,      SPA_POD_Rectangle(&port->current_format->info.h264.size),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&port->current_format->info.h264.framerate),
			0);
		break;
	default:
		return -EIO;
	}

	*param = (struct spa_pod*)spa_pod_builder_pop(builder, &f);

	return 1;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *impl = (struct impl*)object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
		case 0:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_description, SPA_POD_String("The libcamera device"),
				SPA_PROP_INFO_type, SPA_POD_String(impl->device_id.c_str()));
			break;
		case 1:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_deviceName),
				SPA_PROP_INFO_description, SPA_POD_String("The libcamera device name"),
				SPA_PROP_INFO_type, SPA_POD_String(impl->device_name.c_str()));
			break;
		default:
			return spa_libcamera_enum_controls(impl,
					GET_OUT_PORT(impl, 0),
					seq, result.index - 2, num, filter);
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
		case 0:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_device,     SPA_POD_String(impl->device_id.c_str()),
				SPA_PROP_deviceName, SPA_POD_String(impl->device_name.c_str()));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_EnumFormat:
		return spa_libcamera_enum_format(impl, GET_OUT_PORT(impl, 0),
				seq, start, num, filter);
	case SPA_PARAM_Format:
		if ((res = port_get_format(impl, GET_OUT_PORT(impl, 0), result.index, filter, &param, &b)) <= 0)
			return res;
		break;
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_param(void *object,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct spa_pod_object *obj = (struct spa_pod_object *) param;
		struct spa_pod_prop *prop;

		if (param == NULL) {
			impl->device_id.clear();
			impl->device_name.clear();
			return 0;
		}
		SPA_POD_OBJECT_FOREACH(obj, prop) {
			char device[128];

			switch (prop->key) {
			case SPA_PROP_device:
				strncpy(device, (char *)SPA_POD_CONTENTS(struct spa_pod_string, &prop->value),
						sizeof(device)-1);
				impl->device_id = device;
				break;
			default:
				spa_libcamera_set_control(impl, prop);
				break;
			}
		}
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		impl->clock = (struct spa_io_clock*)data;
		break;
	case SPA_IO_Position:
		impl->position = (struct spa_io_position*)data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *impl = (struct impl*)object;
	int res;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
	{
		struct port *port = GET_OUT_PORT(impl, 0);

		if (!port->current_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		if ((res = spa_libcamera_stream_on(impl)) < 0)
			return res;
		break;
	}
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		if ((res = spa_libcamera_stream_off(impl)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_DEVICE_API, "libcamera" },
	{ SPA_KEY_MEDIA_CLASS, "Video/Source" },
	{ SPA_KEY_MEDIA_ROLE, "Camera" },
	{ SPA_KEY_NODE_DRIVER, "true" },
};

static void emit_node_info(struct impl *impl, bool full)
{
	uint64_t old = full ? impl->info.change_mask : 0;
	if (full)
		impl->info.change_mask = impl->info_all;
	if (impl->info.change_mask) {
		struct spa_dict dict = SPA_DICT_INIT_ARRAY(info_items);
		impl->info.props = &dict;
		spa_node_emit_info(&impl->hooks, &impl->info);
		impl->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *impl, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&impl->hooks,
				SPA_DIRECTION_OUTPUT, 0, &port->info);
		port->info.change_mask = old;
	}
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *impl = (struct impl*)object;
	struct spa_hook_list save;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_node_info(impl, true);
	emit_port_info(impl, GET_OUT_PORT(impl, 0), true);

	spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

static int impl_node_set_callbacks(void *object,
				   const struct spa_node_callbacks *callbacks,
				   void *data)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	impl->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_sync(void *object, int seq)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	spa_node_emit_result(&impl->hooks, seq, 0, 0, NULL);

	return 0;
}

static int impl_node_add_port(void *object,
			      enum spa_direction direction,
			      uint32_t port_id, const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object,
		                 enum spa_direction direction,
				 uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_port_enum_params(void *object, int seq,
				      enum spa_direction direction,
				      uint32_t port_id,
				      uint32_t id, uint32_t start, uint32_t num,
				      const struct spa_pod *filter)
{

	struct impl *impl = (struct impl*)object;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	port = GET_PORT(impl, direction, port_id);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
		return spa_libcamera_enum_controls(impl, port, seq, start, num, filter);

	case SPA_PARAM_EnumFormat:
		return spa_libcamera_enum_format(impl, port, seq, start, num, filter);

	case SPA_PARAM_Format:
		if((res = port_get_format(impl, port, result.index, filter, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Buffers:
	{
		if (!port->current_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		/* Get the number of buffers to be used from libcamera and send the same to pipewire
		 * so that exact number of buffers are allocated
		 */
		uint32_t n_buffers = port->streamConfig.bufferCount;

		param = (struct spa_pod*)spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(n_buffers, n_buffers, n_buffers),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(port->streamConfig.frameSize),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->streamConfig.stride));
		break;
	}
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		case 1:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Clock),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_clock)));
			break;
		case 2:
			param = (struct spa_pod*)spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_sequence)));
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

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int port_set_format(struct impl *impl, struct port *port,
			   uint32_t flags, const struct spa_pod *format)
{
	struct spa_video_info info;
	int res;

	if (format == NULL) {
		if (!port->current_format)
			return 0;

		spa_libcamera_stream_off(impl);
		spa_libcamera_clear_buffers(impl, port);
		port->current_format.reset();

		spa_libcamera_close(impl);
		goto done;
	} else {
		spa_zero(info);
		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_video) {
			spa_log_error(impl->log, "media type must be video");
			return -EINVAL;
		}

		switch (info.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			if (spa_format_video_raw_parse(format, &info.info.raw) < 0) {
				spa_log_error(impl->log, "can't parse video raw");
				return -EINVAL;
			}

			if (port->current_format && info.media_type == port->current_format->media_type &&
			    info.media_subtype == port->current_format->media_subtype &&
			    info.info.raw.format == port->current_format->info.raw.format &&
			    info.info.raw.size.width == port->current_format->info.raw.size.width &&
			    info.info.raw.size.height == port->current_format->info.raw.size.height &&
			    info.info.raw.flags == port->current_format->info.raw.flags &&
			    (!(info.info.raw.flags & SPA_VIDEO_FLAG_MODIFIER) ||
			     (info.info.raw.modifier == port->current_format->info.raw.modifier)))
				return 0;
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			if (spa_format_video_mjpg_parse(format, &info.info.mjpg) < 0)
				return -EINVAL;

			if (port->current_format && info.media_type == port->current_format->media_type &&
			    info.media_subtype == port->current_format->media_subtype &&
			    info.info.mjpg.size.width == port->current_format->info.mjpg.size.width &&
			    info.info.mjpg.size.height == port->current_format->info.mjpg.size.height)
				return 0;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			if (spa_format_video_h264_parse(format, &info.info.h264) < 0)
				return -EINVAL;

			if (port->current_format && info.media_type == port->current_format->media_type &&
			    info.media_subtype == port->current_format->media_subtype &&
			    info.info.h264.size.width == port->current_format->info.h264.size.width &&
			    info.info.h264.size.height == port->current_format->info.h264.size.height)
				return 0;
			break;
		default:
			return -EINVAL;
		}
	}

	if (port->current_format && !(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		spa_libcamera_use_buffers(impl, port, NULL, 0);
		port->current_format.reset();
	}

	if (spa_libcamera_set_format(impl, port, &info, flags & SPA_NODE_PARAM_FLAG_TEST_ONLY) < 0)
		return -EINVAL;

	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		port->current_format = info;
	}

    done:
	impl->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->current_format) {
		impl->params[NODE_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		impl->params[NODE_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}
	emit_port_info(impl, port, false);
	emit_node_info(impl, false);

	return 0;
}

static int impl_node_port_set_param(void *object,
				    enum spa_direction direction, uint32_t port_id,
				    uint32_t id, uint32_t flags,
				    const struct spa_pod *param)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(object != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	port = GET_PORT(impl, direction, port_id);

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(impl, port, flags, param);
		break;
	default:
		res = -ENOENT;
	}
	return res;
}

static int impl_node_port_use_buffers(void *object,
				      enum spa_direction direction,
				      uint32_t port_id,
				      uint32_t flags,
				      struct spa_buffer **buffers,
				      uint32_t n_buffers)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	port = GET_PORT(impl, direction, port_id);

	if (port->n_buffers) {
		spa_libcamera_stream_off(impl);
		if ((res = spa_libcamera_clear_buffers(impl, port)) < 0)
			return res;
	}
	if (n_buffers > 0 && !port->current_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;
	if (buffers == NULL)
		return 0;

	if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC) {
		res = spa_libcamera_alloc_buffers(impl, port, buffers, n_buffers);
	} else {
		res = spa_libcamera_use_buffers(impl, port, buffers, n_buffers);
	}
	return res;
}

static int impl_node_port_set_io(void *object,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t id,
				 void *data, size_t size)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	port = GET_PORT(impl, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = (struct spa_io_buffers*)data;
		break;
	case SPA_IO_Control:
		port->control = (struct spa_io_sequence*)data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(void *object,
				       uint32_t port_id,
				       uint32_t buffer_id)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(impl != NULL, -EINVAL);
	spa_return_val_if_fail(port_id == 0, -EINVAL);

	port = GET_OUT_PORT(impl, port_id);

	spa_return_val_if_fail(buffer_id < port->n_buffers, -EINVAL);

	res = spa_libcamera_buffer_recycle(impl, port, buffer_id);

	return res;
}

static int process_control(struct impl *impl, struct spa_pod_sequence *control)
{
	struct spa_pod_control *c;

	SPA_POD_SEQUENCE_FOREACH(control, c) {
		switch (c->type) {
		case SPA_CONTROL_Properties:
		{
			struct spa_pod_prop *prop;
			struct spa_pod_object *obj = (struct spa_pod_object *) &c->value;

			SPA_POD_OBJECT_FOREACH(obj, prop) {
				spa_libcamera_set_control(impl, prop);
			}
			break;
		}
		default:
			break;
		}
	}
	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *impl = (struct impl*)object;
	int res;
	struct spa_io_buffers *io;
	struct port *port;
	struct buffer *b;

	spa_return_val_if_fail(impl != NULL, -EINVAL);

	port = GET_OUT_PORT(impl, 0);
	if ((io = port->io) == NULL)
		return -EIO;

	if (port->control)
		process_control(impl, &port->control->sequence);

	spa_log_trace(impl->log, "%p; status %d", impl, io->status);

	if (io->status == SPA_STATUS_HAVE_DATA) {
		return SPA_STATUS_HAVE_DATA;
	}

	if (io->buffer_id < port->n_buffers) {
		if ((res = spa_libcamera_buffer_recycle(impl, port, io->buffer_id)) < 0)
			return res;

		io->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&port->queue)) {
		return SPA_STATUS_OK;
	}

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);

	spa_log_trace(impl->log, "%p: dequeue buffer %d", impl, b->id);

	io->buffer_id = b->id;
	io->status = SPA_STATUS_HAVE_DATA;

	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
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
	struct impl *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &impl->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	std::destroy_at(reinterpret_cast<impl *>(handle));
	return 0;
}

impl::impl(spa_log *log, spa_loop *data_loop, spa_system *system,
	   std::shared_ptr<CameraManager> manager, std::shared_ptr<Camera> camera, std::string device_id)
	: handle({ SPA_VERSION_HANDLE, impl_get_interface, impl_clear }),
	  log(log),
	  data_loop(data_loop),
	  system(system),
	  device_id(std::move(device_id)),
	  out_ports{{this}},
	  manager(std::move(manager)),
	  camera(std::move(camera))
{
	libcamera_log_topic_init(log);

	spa_hook_list_init(&hooks);

	node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	params[NODE_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	params[NODE_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	params[NODE_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	params[NODE_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);

	info.max_output_ports = 1;
	info.flags = SPA_NODE_FLAG_RT;
	info.params = params;
	info.n_params = N_NODE_PARAMS;
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
	const char *str;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	auto log = static_cast<spa_log *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log));
	auto data_loop = static_cast<spa_loop *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop));
	auto system = static_cast<spa_system *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System));

	if (!data_loop) {
		spa_log_error(log, "a data_loop is needed");
		return -EINVAL;
	}

	if (!system) {
		spa_log_error(log, "a system is needed");
		return -EINVAL;
	}

	auto manager = libcamera_manager_acquire(res);
	if (!manager) {
		spa_log_error(log, "can't start camera manager: %s", spa_strerror(res));
		return res;
	}

	std::string device_id;
	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_LIBCAMERA_PATH)))
		device_id = str;

	auto camera = manager->get(device_id);
	if (!camera) {
		spa_log_error(log, "unknown camera id %s", device_id.c_str());
		return -ENOENT;
	}

	new (handle) impl(log, data_loop, system,
			  std::move(manager), std::move(camera), std::move(device_id));

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
				    const struct spa_interface_info **info,
				    uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

extern "C" {
const struct spa_handle_factory spa_libcamera_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_LIBCAMERA_SOURCE,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
}
