/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/*                         @author Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <poll.h>
#include <limits.h>

#include <linux/media.h>
#include <libcamera/control_ids.h>

int spa_libcamera_open(struct impl *impl)
{
	if (impl->acquired)
		return 0;

	spa_log_info(impl->log, "open camera %s", impl->device_id.c_str());
	impl->camera->acquire();

	impl->allocator = new FrameBufferAllocator(impl->camera);

	impl->acquired = true;
	return 0;
}

int spa_libcamera_close(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	if (!impl->acquired)
		return 0;
	if (impl->active || port->current_format)
		return 0;

	spa_log_info(impl->log, "close camera %s", impl->device_id.c_str());
	delete impl->allocator;
	impl->allocator = nullptr;

	impl->camera->release();

	impl->acquired = false;
	return 0;
}

static void spa_libcamera_get_config(struct impl *impl)
{
	if (impl->config)
		return;

	impl->config = impl->camera->generateConfiguration({ StreamRole::VideoRecording });
}

static int spa_libcamera_buffer_recycle(struct impl *impl, struct port *port, uint32_t buffer_id)
{
	struct buffer *b = &port->buffers[buffer_id];
	int res;

	if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING))
		return 0;

	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUTSTANDING);

	if (buffer_id >= impl->requestPool.size()) {
		spa_log_warn(impl->log, "invalid buffer_id %u >= %zu",
				buffer_id, impl->requestPool.size());
		return -EINVAL;
	}
	Request *request = impl->requestPool[buffer_id].get();
	Stream *stream = port->streamConfig.stream();
	FrameBuffer *buffer = impl->allocator->buffers(stream)[buffer_id].get();
	if ((res = request->addBuffer(stream, buffer)) < 0) {
		spa_log_warn(impl->log, "can't add buffer %u for request: %s",
				buffer_id, spa_strerror(res));
		return -ENOMEM;
	}
	if (!impl->active) {
		impl->pendingRequests.push_back(request);
		return 0;
	} else {
		request->controls().merge(impl->ctrls);
		impl->ctrls.clear();
		if ((res = impl->camera->queueRequest(request)) < 0) {
			spa_log_warn(impl->log, "can't queue buffer %u: %s",
				buffer_id, spa_strerror(res));
			return res == -EACCES ? -EBUSY : res;
		}
	}
	return 0;
}

static int allocBuffers(struct impl *impl, struct port *port, unsigned int count)
{
	int res;

	if ((res = impl->allocator->allocate(port->streamConfig.stream())) < 0)
		return res;

	for (unsigned int i = 0; i < count; i++) {
		std::unique_ptr<Request> request = impl->camera->createRequest(i);
		if (!request) {
			impl->requestPool.clear();
			return -ENOMEM;
		}
		impl->requestPool.push_back(std::move(request));
	}
	return res;
}

static void freeBuffers(struct impl *impl, struct port *port)
{
	impl->pendingRequests.clear();
	impl->requestPool.clear();
	impl->allocator->free(port->streamConfig.stream());
}

static int spa_libcamera_clear_buffers(struct impl *impl, struct port *port)
{
	uint32_t i;

	if (port->n_buffers == 0)
		return 0;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		b = &port->buffers[i];
		d = b->outbuf->datas;

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			spa_log_debug(impl->log, "queueing outstanding buffer %p", b);
			spa_libcamera_buffer_recycle(impl, port, i);
		}
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			munmap(SPA_PTROFF(b->ptr, -d[0].mapoffset, void),
					d[0].maxsize - d[0].mapoffset);
		}
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_ALLOCATED)) {
			close(d[0].fd);
		}
		d[0].type = SPA_ID_INVALID;
	}

	freeBuffers(impl, port);
	port->n_buffers = 0;

	return 0;
}

struct format_info {
	PixelFormat pix;
	uint32_t format;
	uint32_t media_type;
	uint32_t media_subtype;
};

#define MAKE_FMT(pix,fmt,mt,mst) { pix, SPA_VIDEO_FORMAT_ ##fmt, SPA_MEDIA_TYPE_ ##mt, SPA_MEDIA_SUBTYPE_ ##mst }
static const struct format_info format_info[] = {
	/* RGB formats */
	MAKE_FMT(formats::RGB565, RGB16, video, raw),
	MAKE_FMT(formats::RGB565_BE, RGB16, video, raw),
	MAKE_FMT(formats::RGB888, RGB, video, raw),
	MAKE_FMT(formats::BGR888, BGR, video, raw),
	MAKE_FMT(formats::XRGB8888, xRGB, video, raw),
	MAKE_FMT(formats::XBGR8888, xBGR, video, raw),
	MAKE_FMT(formats::RGBX8888, RGBx, video, raw),
	MAKE_FMT(formats::BGRX8888, BGRx, video, raw),
	MAKE_FMT(formats::ARGB8888, ARGB, video, raw),
	MAKE_FMT(formats::ABGR8888, ABGR, video, raw),
	MAKE_FMT(formats::RGBA8888, RGBA, video, raw),
	MAKE_FMT(formats::BGRA8888, BGRA, video, raw),

	MAKE_FMT(formats::YUYV, YUY2, video, raw),
	MAKE_FMT(formats::YVYU, YVYU, video, raw),
	MAKE_FMT(formats::UYVY, UYVY, video, raw),
	MAKE_FMT(formats::VYUY, VYUY, video, raw),

	MAKE_FMT(formats::NV12, NV12, video, raw),
	MAKE_FMT(formats::NV21, NV21, video, raw),
	MAKE_FMT(formats::NV16, NV16, video, raw),
	MAKE_FMT(formats::NV61, NV61, video, raw),
	MAKE_FMT(formats::NV24, NV24, video, raw),

	MAKE_FMT(formats::YUV420, I420, video, raw),
	MAKE_FMT(formats::YVU420, YV12, video, raw),
	MAKE_FMT(formats::YUV422, Y42B, video, raw),

	MAKE_FMT(formats::MJPEG, ENCODED, video, mjpg),
#undef MAKE_FMT
};

static const struct format_info *video_format_to_info(const PixelFormat &pix) {
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].pix == pix)
			return &format_info[i];
	}
	return NULL;
}

static const struct format_info *find_format_info_by_media_type(uint32_t type,
		uint32_t subtype, uint32_t format, int startidx)
{
	size_t i;

	for (i = startidx; i < SPA_N_ELEMENTS(format_info); i++) {
		if ((format_info[i].media_type == type) &&
		    (format_info[i].media_subtype == subtype) &&
		    (format == 0 || format_info[i].format == format))
			return &format_info[i];
	}
	return NULL;
}

static int score_size(Size &a, Size &b)
{
	int x, y;
	x = (int)a.width - (int)b.width;
	y = (int)a.height - (int)b.height;
	return x * x + y * y;
}

static int
spa_libcamera_enum_format(struct impl *impl, struct port *port, int seq,
		     uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	int res;
	const struct format_info *info;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	struct spa_pod *fmt;
	uint32_t i, count = 0, num_sizes;
	PixelFormat format;
	Size frameSize;
	SizeRange sizeRange = SizeRange();

	spa_libcamera_get_config(impl);

	const StreamConfiguration& streamConfig = impl->config->at(0);
	const StreamFormats &formats = streamConfig.formats();

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

	if (result.next == 0) {
		port->fmt_index = 0;
		port->size_index = 0;
	}
next:
	result.index = result.next++;

next_fmt:
	if (port->fmt_index >= formats.pixelformats().size())
		goto enum_end;

	format = formats.pixelformats()[port->fmt_index];

	spa_log_debug(impl->log, "format: %s", format.toString().c_str());

	info = video_format_to_info(format);
	if (info == NULL) {
		spa_log_debug(impl->log, "unknown format");
		port->fmt_index++;
		goto next_fmt;
	}

	num_sizes = formats.sizes(format).size();
	if (num_sizes > 0 && port->size_index <= num_sizes) {
		if (port->size_index == 0) {
			Size wanted = Size(640, 480), test;
			int score, best = INT_MAX;
			for (i = 0; i < num_sizes; i++) {
				test = formats.sizes(format)[i];
				score = score_size(wanted, test);
				if (score < best) {
					best = score;
					frameSize = test;
				}
			}
		}
		else {
			frameSize = formats.sizes(format)[port->size_index - 1];
		}
	} else if (port->size_index < 1) {
		sizeRange = formats.range(format);
		if (sizeRange.hStep == 0 || sizeRange.vStep == 0) {
			port->size_index = 0;
			port->fmt_index++;
			goto next_fmt;
		}
	} else {
		port->size_index = 0;
		port->fmt_index++;
		goto next_fmt;
	}
	port->size_index++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(info->media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(info->media_subtype),
			0);

	if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
		spa_pod_builder_id(&b, info->format);
	}
	if (info->pix.modifier()) {
		spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, 0);
		spa_pod_builder_long(&b, info->pix.modifier());
	}
	spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_size, 0);

	if (sizeRange.hStep != 0 && sizeRange.vStep != 0) {
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Step, 0);
		spa_pod_builder_frame(&b, &f[1]);
		spa_pod_builder_rectangle(&b,
				sizeRange.min.width,
				sizeRange.min.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.min.width,
				sizeRange.min.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.max.width,
				sizeRange.max.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.hStep,
				sizeRange.vStep);
		spa_pod_builder_pop(&b, &f[1]);

	} else {
		spa_pod_builder_rectangle(&b, frameSize.width, frameSize.height);
	}

	fmt = (struct spa_pod*) spa_pod_builder_pop(&b, &f[0]);

	if (spa_pod_filter(&b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
	return res;
}

static int spa_libcamera_set_format(struct impl *impl, struct port *port,
		struct spa_video_info *format, bool try_only)
{
	const struct format_info *info = NULL;
	uint32_t video_format;
	struct spa_rectangle *size = NULL;
	struct spa_fraction *framerate = NULL;
	CameraConfiguration::Status validation;
	int res;

	switch (format->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		video_format = format->info.raw.format;
		size = &format->info.raw.size;
		framerate = &format->info.raw.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.mjpg.size;
		framerate = &format->info.mjpg.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.h264.size;
		framerate = &format->info.h264.framerate;
		break;
	default:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		break;
	}

	info = find_format_info_by_media_type(format->media_type,
					      format->media_subtype, video_format, 0);
	if (info == NULL || size == NULL || framerate == NULL) {
		spa_log_error(impl->log, "unknown media type %d %d %d", format->media_type,
			      format->media_subtype, video_format);
		return -EINVAL;
	}
	StreamConfiguration& streamConfig = impl->config->at(0);

	streamConfig.pixelFormat = info->pix;
	streamConfig.size.width = size->width;
	streamConfig.size.height = size->height;
	streamConfig.bufferCount = 8;

	validation = impl->config->validate();
	if (validation == CameraConfiguration::Invalid)
		return -EINVAL;

	if (try_only)
		return 0;

	if ((res = spa_libcamera_open(impl)) < 0)
		return res;

	res = impl->camera->configure(impl->config.get());
	if (res != 0)
		goto error;

	port->streamConfig = impl->config->at(0);

	if ((res = allocBuffers(impl, port, port->streamConfig.bufferCount)) < 0)
		goto error;

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE;
	port->info.flags = SPA_PORT_FLAG_CAN_ALLOC_BUFFERS |
		SPA_PORT_FLAG_LIVE |
		SPA_PORT_FLAG_PHYSICAL |
		SPA_PORT_FLAG_TERMINAL;
	port->info.rate = SPA_FRACTION(port->rate.num, port->rate.denom);

	return 0;
error:
	spa_libcamera_close(impl);
	return res;

}

static struct {
	uint32_t id;
	uint32_t spa_id;
} control_map[] = {
	{ libcamera::controls::BRIGHTNESS, SPA_PROP_brightness },
	{ libcamera::controls::CONTRAST, SPA_PROP_contrast },
	{ libcamera::controls::SATURATION, SPA_PROP_saturation },
	{ libcamera::controls::EXPOSURE_TIME, SPA_PROP_exposure },
	{ libcamera::controls::ANALOGUE_GAIN, SPA_PROP_gain },
	{ libcamera::controls::SHARPNESS, SPA_PROP_sharpness },
};

static uint32_t control_to_prop_id(struct impl *impl, uint32_t control_id)
{
	SPA_FOR_EACH_ELEMENT_VAR(control_map, c) {
		if (c->id == control_id)
			return c->spa_id;
	}
	return SPA_PROP_START_CUSTOM + control_id;
}

static uint32_t prop_id_to_control(struct impl *impl, uint32_t prop_id)
{
	SPA_FOR_EACH_ELEMENT_VAR(control_map, c) {
		if (c->spa_id == prop_id)
			return c->id;
	}
	if (prop_id >= SPA_PROP_START_CUSTOM)
		return prop_id - SPA_PROP_START_CUSTOM;
	return SPA_ID_INVALID;
}

static int
spa_libcamera_enum_controls(struct impl *impl, struct port *port, int seq,
		       uint32_t start, uint32_t num,
		       const struct spa_pod *filter)
{
	const ControlInfoMap &info = impl->camera->controls();
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	struct spa_pod *ctrl;
	uint32_t count = 0, skip, id;
	int res;
	const ControlId *ctrl_id;
	ControlInfo ctrl_info;

	result.id = SPA_PARAM_PropInfo;
	result.next = start;

	auto it = info.begin();
	for (skip = result.next; skip; skip--)
		it++;

	if (false) {
next:
		it++;
	}
	result.index = result.next++;
	if (it == info.end())
		goto enum_end;

	ctrl_id = it->first;
	ctrl_info = it->second;

	id = control_to_prop_id(impl, ctrl_id->id());

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(&b,
			SPA_PROP_INFO_id,   SPA_POD_Id(id),
			SPA_PROP_INFO_description, SPA_POD_String(ctrl_id->name().c_str()),
			0);

	switch (ctrl_id->type()) {
	case ControlTypeBool: {
		bool def;
		if (ctrl_info.def().isNone())
			def = ctrl_info.min().get<bool>();
		else
			def = ctrl_info.def().get<bool>();

		spa_pod_builder_add(&b,
					SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(
							def),
					0);
	} break;
	case ControlTypeFloat: {
		float min = ctrl_info.min().get<float>();
		float max = ctrl_info.max().get<float>();
		float def;

		if (ctrl_info.def().isNone())
			def = (min + max) / 2;
		else
			def = ctrl_info.def().get<float>();

		spa_pod_builder_add(&b,
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
						def, min, max),
				0);
	} break;
	case ControlTypeInteger32: {
		int32_t min = ctrl_info.min().get<int32_t>();
		int32_t max = ctrl_info.max().get<int32_t>();
		int32_t def;

		if (ctrl_info.def().isNone())
			def = (min + max) / 2;
		else
			def = ctrl_info.def().get<int32_t>();

		spa_pod_builder_add(&b,
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(
						def, min, max),
				0);
	} break;
	default:
		goto next;
	}

	ctrl = (struct spa_pod*) spa_pod_builder_pop(&b, &f[0]);

	if (spa_pod_filter(&b, &result.param, ctrl, filter) < 0)
		goto next;

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

enum_end:
	res = 0;
	return res;
}

struct val {
	uint32_t type;
	float f_val;
	int32_t i_val;
	bool b_val;
	uint32_t id;
};

static int do_update_ctrls(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *impl = (struct impl *)user_data;
	const struct val *d = (const struct val *)data;
	switch (d->type) {
	case ControlTypeBool:
		impl->ctrls.set(d->id, d->b_val);
		break;
	case ControlTypeFloat:
		impl->ctrls.set(d->id, d->f_val);
		break;
	case ControlTypeInteger32:
		//impl->ctrls.set(d->id, (int32_t)d->i_val);
		break;
	default:
		break;
	}
	return 0;
}

static int
spa_libcamera_set_control(struct impl *impl, const struct spa_pod_prop *prop)
{
	const ControlInfoMap &info = impl->camera->controls();
	const ControlId *ctrl_id;
	int res;
	struct val d;
	uint32_t control_id;

	control_id = prop_id_to_control(impl, prop->key);
	if (control_id == SPA_ID_INVALID)
		return -ENOENT;

	auto v = info.idmap().find(control_id);
	if (v == info.idmap().end())
		return -ENOENT;

	ctrl_id = v->second;

	d.type = ctrl_id->type();
	d.id = ctrl_id->id();

	switch (d.type) {
	case ControlTypeBool:
		if ((res = spa_pod_get_bool(&prop->value, &d.b_val)) < 0)
			goto done;
		break;
	case ControlTypeFloat:
		if ((res = spa_pod_get_float(&prop->value, &d.f_val)) < 0)
			goto done;
		break;
	case ControlTypeInteger32:
		if ((res = spa_pod_get_int(&prop->value, &d.i_val)) < 0)
			goto done;
		break;
	default:
		res = -EINVAL;
		goto done;
	}
	spa_loop_invoke(impl->data_loop, do_update_ctrls, 0, &d, sizeof(d), true, impl);
	res = 0;
done:
	return res;
}


static void libcamera_on_fd_events(struct spa_source *source)
{
	struct impl *impl = (struct impl*) source->data;
	struct spa_io_buffers *io;
	struct port *port = &impl->out_ports[0];
	uint32_t index, buffer_id;
	struct buffer *b;
	uint64_t cnt;

	if (source->rmask & SPA_IO_ERR) {
		spa_log_error(impl->log, "libcamera %p: error %08x", impl, source->rmask);
		if (impl->source.loop)
			spa_loop_remove_source(impl->data_loop, &impl->source);
		return;
	}

	if (!(source->rmask & SPA_IO_IN)) {
		spa_log_warn(impl->log, "libcamera %p: spurious wakeup %d", impl, source->rmask);
		return;
	}

	if (spa_system_eventfd_read(impl->system, impl->source.fd, &cnt) < 0) {
		spa_log_error(impl->log, "Failed to read on event fd");
		return;
	}

	if (spa_ringbuffer_get_read_index(&port->ring, &index) < 1) {
		spa_log_error(impl->log, "nothing is queued");
		return;
	}
	buffer_id = port->ring_ids[index & MASK_BUFFERS];
	spa_ringbuffer_read_update(&port->ring, index + 1);

	b = &port->buffers[buffer_id];
	spa_list_append(&port->queue, &b->link);

	io = port->io;
	if (io == NULL) {
		b = spa_list_first(&port->queue, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);
		spa_libcamera_buffer_recycle(impl, port, b->id);
	} else if (io->status != SPA_STATUS_HAVE_DATA) {
		if (io->buffer_id < port->n_buffers)
			spa_libcamera_buffer_recycle(impl, port, io->buffer_id);

		b = spa_list_first(&port->queue, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
		spa_log_trace(impl->log, "libcamera %p: now queued %d", impl, b->id);
	}
	spa_node_call_ready(&impl->callbacks, SPA_STATUS_HAVE_DATA);
}

static int spa_libcamera_use_buffers(struct impl *impl, struct port *port,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	return -ENOTSUP;
}

static const struct {
	Orientation libcamera_orientation; /* clockwise rotation then horizontal mirroring */
	uint32_t spa_transform_value; /* horizontal mirroring then counter-clockwise rotation */
} orientation_map[] = {
	{ Orientation::Rotate0, SPA_META_TRANSFORMATION_None },
	{ Orientation::Rotate0Mirror, SPA_META_TRANSFORMATION_Flipped },
	{ Orientation::Rotate90, SPA_META_TRANSFORMATION_270 },
	{ Orientation::Rotate90Mirror, SPA_META_TRANSFORMATION_Flipped90 },
	{ Orientation::Rotate180, SPA_META_TRANSFORMATION_180 },
	{ Orientation::Rotate180Mirror, SPA_META_TRANSFORMATION_Flipped180 },
	{ Orientation::Rotate270, SPA_META_TRANSFORMATION_90 },
	{ Orientation::Rotate270Mirror, SPA_META_TRANSFORMATION_Flipped270 },
};

static uint32_t libcamera_orientation_to_spa_transform_value(Orientation orientation)
{
	for (const auto& t : orientation_map) {
		if (t.libcamera_orientation == orientation)
			return t.spa_transform_value;
	}
	return SPA_META_TRANSFORMATION_None;
}

static int
mmap_init(struct impl *impl, struct port *port,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	unsigned int i, j;
	struct spa_data *d;
	Stream *stream = impl->config->at(0).stream();
	const std::vector<std::unique_ptr<FrameBuffer>> &bufs =
			impl->allocator->buffers(stream);

	if (n_buffers > 0) {
		if (bufs.size() != n_buffers)
			return -EINVAL;

		d = buffers[0]->datas;

		if (d[0].type != SPA_ID_INVALID &&
		    d[0].type & (1u << SPA_DATA_DmaBuf)) {
			port->memtype = SPA_DATA_DmaBuf;
		} else if (d[0].type != SPA_ID_INVALID &&
		    d[0].type & (1u << SPA_DATA_MemFd)) {
			port->memtype = SPA_DATA_MemFd;
		} else if (d[0].type & (1u << SPA_DATA_MemPtr)) {
			port->memtype = SPA_DATA_MemPtr;
		} else {
			spa_log_error(impl->log, "v4l2: can't use buffers of type %d", d[0].type);
			return -EINVAL;
		}
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(impl->log, "invalid buffer data");
			return -EINVAL;
		}

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = (struct spa_meta_header*)spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		b->videotransform = (struct spa_meta_videotransform*)spa_buffer_find_meta_data(
			buffers[i], SPA_META_VideoTransform, sizeof(*b->videotransform));
		if (b->videotransform) {
			b->videotransform->transform =
				libcamera_orientation_to_spa_transform_value(impl->config->orientation);
			spa_log_debug(impl->log, "Setting videotransform for buffer %u to %u",
				i, b->videotransform->transform);

		}

		d = buffers[i]->datas;
		for(j = 0; j < buffers[i]->n_datas; ++j) {
			d[j].type = port->memtype;
			d[j].flags = SPA_DATA_FLAG_READABLE;
			d[j].mapoffset = 0;
			d[j].maxsize = port->streamConfig.frameSize;
			d[j].chunk->offset = 0;
			d[j].chunk->size = port->streamConfig.frameSize;
			d[j].chunk->stride = port->streamConfig.stride;
			d[j].chunk->flags = 0;

			if (port->memtype == SPA_DATA_DmaBuf ||
			    port->memtype == SPA_DATA_MemFd) {
				d[j].flags |= SPA_DATA_FLAG_MAPPABLE;
				d[j].fd = bufs[i]->planes()[j].fd.get();
				spa_log_debug(impl->log, "Got fd = %ld for buffer: #%d", d[j].fd, i);
				d[j].data = NULL;
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_ALLOCATED);
			}
			else if(port->memtype == SPA_DATA_MemPtr) {
				d[j].fd = -1;
				d[j].data = mmap(NULL,
						d[j].maxsize + d[j].mapoffset,
						PROT_READ, MAP_SHARED,
						bufs[i]->planes()[j].fd.get(),
						0);
				if (d[j].data == MAP_FAILED) {
					spa_log_error(impl->log, "mmap: %m");
					continue;
				}
				b->ptr = d[j].data;
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				spa_log_debug(impl->log, "mmap ptr:%p", d[j].data);
			} else {
				spa_log_error(impl->log, "invalid buffer type");
				return -EIO;
			}
		}
		spa_libcamera_buffer_recycle(impl, port, i);
	}
	port->n_buffers = n_buffers;
	spa_log_debug(impl->log, "we have %d buffers", n_buffers);

	return 0;
}

static int
spa_libcamera_alloc_buffers(struct impl *impl, struct port *port,
		       struct spa_buffer **buffers,
		       uint32_t n_buffers)
{
	int res;

	if (port->n_buffers > 0)
		return -EIO;

	if ((res = mmap_init(impl, port, buffers, n_buffers)) < 0)
		return res;

	return 0;
}


void impl::requestComplete(libcamera::Request *request)
{
	struct impl *impl = this;
	struct port *port = &impl->out_ports[0];
	Stream *stream = port->streamConfig.stream();
	uint32_t index, buffer_id;
	struct buffer *b;

	spa_log_debug(impl->log, "request complete");

	buffer_id = request->cookie();
	b = &port->buffers[buffer_id];

	if ((request->status() == Request::RequestCancelled)) {
		spa_log_debug(impl->log, "Request was cancelled");
		request->reuse();
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);
		spa_libcamera_buffer_recycle(impl, port, b->id);
		return;
	}
	FrameBuffer *buffer = request->findBuffer(stream);
	if (buffer == nullptr) {
		spa_log_warn(impl->log, "unknown buffer");
		return;
	}
	const FrameMetadata &fmd = buffer->metadata();

	if (impl->clock) {
		/* FIXME, we should follow the driver clock and target_ values.
		 * for now we ignore and use our own. */
		impl->clock->target_rate = port->rate;
		impl->clock->target_duration = 1;

		impl->clock->nsec = fmd.timestamp;
		impl->clock->rate = port->rate;
		impl->clock->position = fmd.sequence;
		impl->clock->duration = 1;
		impl->clock->delay = 0;
		impl->clock->rate_diff = 1.0;
		impl->clock->next_nsec = fmd.timestamp;
	}
	if (b->h) {
		b->h->flags = 0;
		b->h->offset = 0;
		b->h->seq = fmd.sequence;
		b->h->pts = fmd.timestamp;
		b->h->dts_offset = 0;
	}
	request->reuse();

	spa_ringbuffer_get_write_index(&port->ring, &index);
	port->ring_ids[index & MASK_BUFFERS] = buffer_id;
	spa_ringbuffer_write_update(&port->ring, index + 1);

	if (spa_system_eventfd_write(impl->system, impl->source.fd, 1) < 0)
		spa_log_error(impl->log, "Failed to write on event fd");

}

static int spa_libcamera_stream_on(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	int res;

	if (!port->current_format) {
		spa_log_error(impl->log, "Exting %s with -EIO", __FUNCTION__);
		return -EIO;
	}

	if (impl->active)
		return 0;

	impl->camera->requestCompleted.connect(impl, &impl::requestComplete);

	spa_log_info(impl->log, "starting camera %s", impl->device_id.c_str());
	if ((res = impl->camera->start()) < 0)
		goto error;

	for (Request *req : impl->pendingRequests) {
		if ((res = impl->camera->queueRequest(req)) < 0)
			goto error_stop;
	}
	impl->pendingRequests.clear();

	impl->source.func = libcamera_on_fd_events;
	impl->source.data = impl;
	impl->source.fd = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	impl->source.mask = SPA_IO_IN | SPA_IO_ERR;
	impl->source.rmask = 0;
	if (impl->source.fd < 0) {
		spa_log_error(impl->log, "Failed to create eventfd: %s", spa_strerror(impl->source.fd));
		res = impl->source.fd;
		goto error_stop;
	}
	spa_loop_add_source(impl->data_loop, &impl->source);

	impl->active = true;

	return 0;

error_stop:
	impl->camera->stop();
error:
	impl->camera->requestCompleted.disconnect(impl, &impl::requestComplete);
	return res == -EACCES ? -EBUSY : res;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *impl = (struct impl *)user_data;
	if (impl->source.loop)
		spa_loop_remove_source(loop, &impl->source);
	return 0;
}

static int spa_libcamera_stream_off(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	int res;

	if (!impl->active) {
		for (std::unique_ptr<Request> &req : impl->requestPool)
			req->reuse();
		return 0;
	}

	impl->active = false;
	spa_log_info(impl->log, "stopping camera %s", impl->device_id.c_str());
	impl->pendingRequests.clear();

	if ((res = impl->camera->stop()) < 0) {
		spa_log_warn(impl->log, "error stopping camera %s: %s",
				impl->device_id.c_str(), spa_strerror(res));
	}

	impl->camera->requestCompleted.disconnect(impl, &impl::requestComplete);

	spa_loop_invoke(impl->data_loop, do_remove_source, 0, NULL, 0, true, impl);
	if (impl->source.fd >= 0)  {
		spa_system_close(impl->system, impl->source.fd);
		impl->source.fd = -1;
	}

	spa_list_init(&port->queue);

	return 0;
}
