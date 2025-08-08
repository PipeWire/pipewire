/* Spa libcamera source */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/*                         @author Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include <sys/mman.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/monitor/device.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/control/control.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/filter.h>

#include <libcamera/camera.h>
#include <libcamera/control_ids.h>
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
#define BUFFER_FLAG_MAPPED	(1<<1)

struct buffer {
	uint32_t id;
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	struct spa_meta_videotransform *videotransform;
	void *ptr;
};

struct port {
	struct impl *impl;

	std::optional<spa_video_info> current_format;

	struct spa_fraction rate = {};
	StreamConfiguration streamConfig;

	spa_data_type memtype = SPA_DATA_Invalid;
	uint32_t buffers_blocks = 1;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers = 0;
	struct spa_list queue;

	static constexpr uint64_t info_all = SPA_PORT_CHANGE_MASK_FLAGS |
		SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS;
	struct spa_port_info info = SPA_PORT_INFO_INIT();
	struct spa_io_buffers *io = nullptr;
	struct spa_io_sequence *control = nullptr;
	uint32_t control_size;
#define PORT_PropInfo	0
#define PORT_EnumFormat	1
#define PORT_Meta	2
#define PORT_IO		3
#define PORT_Format	4
#define PORT_Buffers	5
#define PORT_Latency	6
#define N_PORT_PARAMS	7
	struct spa_param_info params[N_PORT_PARAMS];

	std::size_t fmt_index = 0;
	std::size_t size_index = 0;

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
		params[PORT_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READ);

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

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks = {};

	std::array<port, 1> out_ports;

	struct spa_io_position *position = nullptr;
	struct spa_io_clock *clock = nullptr;

	struct spa_latency_info latency[2];

	std::shared_ptr<CameraManager> manager;
	std::shared_ptr<Camera> camera;
	const std::unique_ptr<CameraConfiguration> config;

	FrameBufferAllocator allocator;
	std::vector<std::unique_ptr<libcamera::Request>> requestPool;
	spa_ringbuffer completed_requests_rb = SPA_RINGBUFFER_INIT();
	std::array<libcamera::Request *, MAX_BUFFERS> completed_requests;

	void requestComplete(libcamera::Request *request);

	struct spa_source source = {};

	ControlList ctrls;
	ControlList initial_controls;
	bool active = false;
	bool acquired = false;

	impl(spa_log *log, spa_loop *data_loop, spa_system *system,
	     std::shared_ptr<CameraManager> manager, std::shared_ptr<Camera> camera,
	     std::unique_ptr<CameraConfiguration> config);

	struct spa_dll dll;
};

#define CHECK_PORT(impl,direction,port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)

#define GET_OUT_PORT(impl,p)         (&impl->out_ports[p])
#define GET_PORT(impl,d,p)           GET_OUT_PORT(impl,p)

void setup_initial_controls(const ControlInfoMap& ctrl_infos, ControlList& ctrls)
{
	/* Libcamera recommends cameras default to manual focus mode, but we don't
	 * expose any focus controls.  So, specifically enable autofocus on
	 * cameras which support it. */
	auto af_it = ctrl_infos.find(libcamera::controls::AF_MODE);
	if (af_it != ctrl_infos.end()) {
		const ControlInfo &ctrl_info = af_it->second;
		auto is_af_continuous = [](const ControlValue &value) {
			return value.get<int32_t>() == libcamera::controls::AfModeContinuous;
		};
		if (std::any_of(ctrl_info.values().begin(),
		    ctrl_info.values().end(), is_af_continuous)) {
			ctrls.set(libcamera::controls::AF_MODE,
					libcamera::controls::AfModeContinuous);
		}
	}

	auto ae_it = ctrl_infos.find(libcamera::controls::AE_ENABLE);
	if (ae_it != ctrl_infos.end()) {
		ctrls.set(libcamera::controls::AE_ENABLE, true);
	}
}

int spa_libcamera_open(struct impl *impl)
{
	if (impl->acquired)
		return 0;

	spa_log_info(impl->log, "open camera %s", impl->camera->id().c_str());

	if (int res = impl->camera->acquire(); res < 0)
		return res;

	spa_assert(!impl->allocator.allocated());

	const ControlInfoMap &controls = impl->camera->controls();
	setup_initial_controls(controls, impl->initial_controls);

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

	spa_log_info(impl->log, "close camera %s", impl->camera->id().c_str());

	spa_assert(!impl->allocator.allocated());

	impl->camera->release();

	impl->acquired = false;
	return 0;
}

int spa_libcamera_buffer_recycle(struct impl *impl, struct port *port, uint32_t buffer_id)
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
	if (impl->active) {
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

void freeBuffers(struct impl *impl, struct port *port)
{
	impl->requestPool.clear();
	std::ignore = impl->allocator.free(port->streamConfig.stream());
}

[[nodiscard]]
std::size_t count_unique_fds(libcamera::Span<const libcamera::FrameBuffer::Plane> planes)
{
	std::size_t c = 0;
	int fd = -1;

	for (const auto& plane : planes) {
		const int current_fd = plane.fd.get();
		if (current_fd >= 0 && current_fd != fd) {
			c += 1;
			fd = current_fd;
		}
	}

	return c;
}

int allocBuffers(struct impl *impl, struct port *port, unsigned int count)
{
	libcamera::Stream *stream = port->streamConfig.stream();
	int res;

	if (!impl->requestPool.empty())
		return -EBUSY;

	if ((res = impl->allocator.allocate(stream)) < 0)
		return res;

	const auto& bufs = impl->allocator.buffers(stream);
	if (bufs.empty() || bufs.size() != count) {
		res = -ENOBUFS;
		goto err;
	}

	for (std::size_t i = 0; i < bufs.size(); i++) {
		std::unique_ptr<Request> request = impl->camera->createRequest(i);
		if (!request) {
			res = -ENOMEM;
			goto err;
		}

		res = request->addBuffer(stream, bufs[i].get());
		if (res < 0)
			goto err;

		impl->requestPool.push_back(std::move(request));
	}

	/* Some devices require data for each output video frame to be
	 * placed in discontiguous memory buffers. In such cases, one
	 * video frame has to be addressed using more than one memory.
	 * address. Therefore, need calculate the number of discontiguous
	 * memory and allocate the specified amount of memory */
	port->buffers_blocks = count_unique_fds(bufs.front()->planes());
	if (port->buffers_blocks <= 0) {
		res = -ENOBUFS;
		goto err;
	}

	return 0;

err:
	freeBuffers(impl, port);

	return res;
}

int spa_libcamera_clear_buffers(struct impl *impl, struct port *port)
{
	uint32_t i;

	if (port->n_buffers == 0)
		return 0;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		b = &port->buffers[i];
		d = b->outbuf->datas;

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			munmap(SPA_PTROFF(b->ptr, -d[0].mapoffset, void),
					d[0].maxsize - d[0].mapoffset);
		}

		d[0].type = SPA_ID_INVALID;
	}

	port->n_buffers = 0;

	return 0;
}

struct format_info {
	PixelFormat pix;
	spa_video_format format;
	spa_media_type media_type;
	spa_media_subtype media_subtype;
};

#define MAKE_FMT(pix,fmt,mt,mst) { pix, SPA_VIDEO_FORMAT_ ##fmt, SPA_MEDIA_TYPE_ ##mt, SPA_MEDIA_SUBTYPE_ ##mst }
const struct format_info format_info[] = {
	/* RGB formats */
	MAKE_FMT(formats::R8, GRAY8, video, raw),
	MAKE_FMT(formats::RGB565, RGB16, video, raw),
	MAKE_FMT(formats::RGB565_BE, RGB16, video, raw),
	MAKE_FMT(formats::RGB888, BGR, video, raw),
	MAKE_FMT(formats::BGR888, RGB, video, raw),
	MAKE_FMT(formats::XRGB8888, BGRx, video, raw),
	MAKE_FMT(formats::XBGR8888, RGBx, video, raw),
	MAKE_FMT(formats::RGBX8888, xBGR, video, raw),
	MAKE_FMT(formats::BGRX8888, xRGB, video, raw),
	MAKE_FMT(formats::ARGB8888, BGRA, video, raw),
	MAKE_FMT(formats::ABGR8888, RGBA, video, raw),
	MAKE_FMT(formats::RGBA8888, ABGR, video, raw),
	MAKE_FMT(formats::BGRA8888, ARGB, video, raw),

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

const struct format_info *video_format_to_info(const PixelFormat &pix)
{
	for (const auto& f : format_info) {
		if (f.pix == pix)
			return &f;
	}

	return nullptr;
}

const struct format_info *find_format_info_by_media_type(
	uint32_t type, uint32_t subtype, uint32_t format)
{
	for (const auto& f : format_info) {
		if (f.media_type == type && f.media_subtype == subtype
		    && (f.format == SPA_VIDEO_FORMAT_UNKNOWN || f.format == format))
			return &f;
	}

	return nullptr;
}

int score_size(const Size &a, const Size &b)
{
	int x, y;
	x = (int)a.width - (int)b.width;
	y = (int)a.height - (int)b.height;
	return x * x + y * y;
}

[[nodiscard]]
spa_video_colorimetry
color_space_to_colorimetry(const libcamera::ColorSpace& colorspace)
{
	spa_video_colorimetry res = {};

	switch (colorspace.range) {
	case ColorSpace::Range::Full:
		res.range = SPA_VIDEO_COLOR_RANGE_0_255;
		break;
	case ColorSpace::Range::Limited:
		res.range = SPA_VIDEO_COLOR_RANGE_16_235;
		break;
	}

	switch (colorspace.ycbcrEncoding) {
	case ColorSpace::YcbcrEncoding::None:
		res.matrix = SPA_VIDEO_COLOR_MATRIX_RGB;
		break;
	case ColorSpace::YcbcrEncoding::Rec601:
		res.matrix = SPA_VIDEO_COLOR_MATRIX_BT601;
		break;
	case ColorSpace::YcbcrEncoding::Rec709:
		res.matrix = SPA_VIDEO_COLOR_MATRIX_BT709;
		break;
	case ColorSpace::YcbcrEncoding::Rec2020:
		res.matrix = SPA_VIDEO_COLOR_MATRIX_BT2020;
		break;
	}

	switch (colorspace.transferFunction) {
	case ColorSpace::TransferFunction::Linear:
		res.transfer = SPA_VIDEO_TRANSFER_GAMMA10;
		break;
	case ColorSpace::TransferFunction::Srgb:
		res.transfer = SPA_VIDEO_TRANSFER_SRGB;
		break;
	case ColorSpace::TransferFunction::Rec709:
		res.transfer = SPA_VIDEO_TRANSFER_BT709;
		break;
	}

	switch (colorspace.primaries) {
	case ColorSpace::Primaries::Raw:
		res.primaries = SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN;
		break;
	case ColorSpace::Primaries::Smpte170m:
		res.primaries = SPA_VIDEO_COLOR_PRIMARIES_SMPTE170M;
		break;
	case ColorSpace::Primaries::Rec709:
		res.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709;
		break;
	case ColorSpace::Primaries::Rec2020:
		res.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT2020;
		break;
	}

	return res;
}

int
spa_libcamera_enum_format(struct impl *impl, struct port *port, int seq,
		     uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	uint32_t count = 0;

	const StreamConfiguration& streamConfig = impl->config->at(0);
	const StreamFormats &formats = streamConfig.formats();
	const auto &pixel_formats = formats.pixelformats();

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

	if (result.next == 0) {
		port->fmt_index = 0;
		port->size_index = 0;
	}
next:
	result.index = result.next++;

next_fmt:
	if (port->fmt_index >= pixel_formats.size())
		return 0;

	auto format = pixel_formats[port->fmt_index];
	spa_log_debug(impl->log, "format: %s", format.toString().c_str());

	const auto *info = video_format_to_info(format);
	if (info == nullptr) {
		spa_log_debug(impl->log, "unknown format");
		port->fmt_index++;
		goto next_fmt;
	}

	const auto& sizes = formats.sizes(format);
	SizeRange sizeRange;
	Size frameSize;

	if (!sizes.empty() && port->size_index <= sizes.size()) {
		if (port->size_index == 0) {
			Size wanted = Size(640, 480);
			int best = std::numeric_limits<int>::max();

			for (const auto& test : sizes) {
				int score = score_size(wanted, test);
				if (score < best) {
					best = score;
					frameSize = test;
				}
			}
		}
		else {
			frameSize = sizes[port->size_index - 1];
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

	if (streamConfig.colorSpace) {
		auto colorimetry = color_space_to_colorimetry(*streamConfig.colorSpace);

		spa_pod_builder_add(&b,
				SPA_FORMAT_VIDEO_colorRange,
				SPA_POD_Id(colorimetry.range),
				SPA_FORMAT_VIDEO_colorMatrix,
				SPA_POD_Id(colorimetry.matrix),
				SPA_FORMAT_VIDEO_transferFunction,
				SPA_POD_Id(colorimetry.transfer),
				SPA_FORMAT_VIDEO_colorPrimaries,
				SPA_POD_Id(colorimetry.primaries), 0);
	}

	const auto *fmt = reinterpret_cast<spa_pod *>(spa_pod_builder_pop(&b, &f[0]));
	if (spa_pod_filter(&b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

int spa_libcamera_set_format(struct impl *impl, struct port *port,
		struct spa_video_info *format, bool try_only)
{
	const struct format_info *info = nullptr;
	uint32_t video_format;
	struct spa_rectangle *size = nullptr;
	struct spa_fraction *framerate = nullptr;
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
					      format->media_subtype, video_format);
	if (info == nullptr || size == nullptr || framerate == nullptr) {
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

const struct {
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

uint32_t control_to_prop_id(uint32_t control_id)
{
	for (const auto& c : control_map) {
		if (c.id == control_id)
			return c.spa_id;
	}

	return SPA_PROP_START_CUSTOM + control_id;
}

uint32_t prop_id_to_control(uint32_t prop_id)
{
	if (prop_id >= SPA_PROP_START_CUSTOM)
		return prop_id - SPA_PROP_START_CUSTOM;

	for (const auto& c : control_map) {
		if (c.spa_id == prop_id)
			return c.id;
	}

	return SPA_ID_INVALID;
}

[[nodiscard]]
ControlValue control_value_from_pod(const libcamera::ControlId& cid, const spa_pod *value, const void *body)
{
	if (cid.isArray())
		return {};

	switch (cid.type()) {
	case libcamera::ControlTypeBool: {
		bool v;
		if (spa_pod_body_get_bool(value, body, &v) < 0)
			return {};

		return v;
	}
	case libcamera::ControlTypeInteger32: {
		int32_t v;
		if (spa_pod_body_get_int(value, body, &v) < 0)
			return {};

		return v;
	}
	case libcamera::ControlTypeFloat: {
		float v;
		if (spa_pod_body_get_float(value, body, &v) < 0)
			return {};

		return v;
	}
	default:
		return {};
	}

	return {};
}

int control_list_update_from_prop(libcamera::ControlList& list, const spa_pod_prop *prop, const void *body)
{
	auto id = prop_id_to_control(prop->key);
	if (id == SPA_ID_INVALID)
		return -ENOENT;

	auto it = list.idMap()->find(id);
	if (it == list.idMap()->end())
		return -ENOENT;

	if (!list.infoMap()->count(it->second))
		return -ENOENT;

	auto val = control_value_from_pod(*it->second, &prop->value, body);
	if (val.isNone())
		return -EINVAL;

	list.set(id, std::move(val));

	return 0;
}

[[nodiscard]]
bool control_value_to_pod(spa_pod_builder& b, const libcamera::ControlValue& cv)
{
	if (cv.isArray())
		return false;

	switch (cv.type()) {
	case libcamera::ControlTypeBool: {
		spa_pod_builder_bool(&b, cv.get<bool>());
		break;
	}
	case libcamera::ControlTypeInteger32: {
		spa_pod_builder_int(&b, cv.get<int32_t>());
		break;
	}
	case libcamera::ControlTypeFloat: {
		spa_pod_builder_float(&b, cv.get<float>());
		break;
	}
	default:
		return false;
	}

	return true;
}

template<typename T>
[[nodiscard]]
std::array<T, 3> control_info_to_range(const libcamera::ControlInfo& cinfo)
{
	static_assert(std::is_arithmetic_v<T>);

	auto min = cinfo.min().get<T>();
	auto max = cinfo.max().get<T>();
	spa_assert(min <= max);

	auto def = !cinfo.def().isNone()
			? cinfo.def().get<T>()
			: (min + ((max - min) / 2));

	return {{ min, max, def }};
}

[[nodiscard]]
spa_pod *control_details_to_pod(spa_pod_builder& b,
				const libcamera::ControlId& cid, const libcamera::ControlInfo& cinfo)
{
	if (cid.isArray())
		return nullptr;

	auto id = control_to_prop_id(cid.id());
	spa_pod_frame f;

	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(&b,
			SPA_PROP_INFO_id,   SPA_POD_Id(id),
			SPA_PROP_INFO_description, SPA_POD_String(cid.name().c_str()),
			0);

	if (cinfo.values().empty()) {
		switch (cid.type()) {
		case ControlTypeBool: {
			auto min = cinfo.min().get<bool>();
			auto max = cinfo.max().get<bool>();
			auto def = !cinfo.def().isNone()
				? cinfo.def().get<bool>()
				: min;
			spa_pod_frame f;

			spa_pod_builder_prop(&b, SPA_PROP_INFO_type, 0);
			spa_pod_builder_push_choice(&b, &f, SPA_CHOICE_Enum, 0);
			spa_pod_builder_bool(&b, def);
			spa_pod_builder_bool(&b, min);
			if (max != min)
				spa_pod_builder_bool(&b, max);
			spa_pod_builder_pop(&b, &f);
			break;
		}
		case ControlTypeFloat: {
			auto [ min, max, def ] = control_info_to_range<float>(cinfo);

			spa_pod_builder_add(&b,
					SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(
							def, min, max),
					0);
			break;
		}
		case ControlTypeInteger32: {
			auto [ min, max, def ] = control_info_to_range<int32_t>(cinfo);

			spa_pod_builder_add(&b,
					SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(
							def, min, max),
					0);
			break;
		}
		default:
			return nullptr;
		}
	}
	else {
		spa_pod_frame f;

		spa_pod_builder_prop(&b, SPA_PROP_INFO_type, 0);
		spa_pod_builder_push_choice(&b, &f, SPA_CHOICE_Enum, 0);

		if (!control_value_to_pod(b, cinfo.def()))
			return nullptr;

		for (const auto& cv : cinfo.values()) {
			if (!control_value_to_pod(b, cv))
				return nullptr;
		}

		spa_pod_builder_pop(&b, &f);

		if (cid.type() == libcamera::ControlTypeInteger32) {
			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);

			spa_pod_builder_push_struct(&b, &f);
			for (const auto& cv : cinfo.values()) {
				auto it = cid.enumerators().find(cv.get<int32_t>());
				if (it == cid.enumerators().end())
					continue;

				spa_pod_builder_int(&b, it->first);
				spa_pod_builder_string_len(&b, it->second.data(), it->second.size());
			}
			spa_pod_builder_pop(&b, &f);
		}
	}

	return reinterpret_cast<spa_pod *>(spa_pod_builder_pop(&b, &f));
}

int
spa_libcamera_enum_controls(struct impl *impl, struct port *port, int seq,
		       uint32_t start, uint32_t offset, uint32_t num,
		       const struct spa_pod *filter)
{
	const ControlInfoMap &info = impl->camera->controls();
	spa_auto(spa_pod_dynamic_builder) b = {};
	spa_pod_builder_state state;
	uint8_t buffer[4096];
	spa_result_node_params result = {
		.id = SPA_PARAM_PropInfo,
	};

	auto it = info.begin();
	for (auto skip = start - offset; skip && it != info.end(); skip--)
		it++;

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
	spa_pod_builder_get_state(&b.b, &state);

	for (result.index = start; num > 0 && it != info.end(); ++it, result.index++) {
		spa_log_debug(impl->log, "%p: controls[%" PRIu32 "]: %s::%s",
				impl, result.index, it->first->vendor().c_str(),
				it->first->name().c_str());

		spa_pod_builder_reset(&b.b, &state);

		const auto *ctrl = control_details_to_pod(b.b, *it->first, it->second);
		if (!ctrl)
			continue;

		if (spa_pod_filter(&b.b, &result.param, ctrl, filter) < 0)
			continue;

		result.next = result.index + 1;

		spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
		num -= 1;
	}

	return 0;
}

int spa_libcamera_apply_controls(struct impl *impl, libcamera::ControlList&& controls)
{
	if (controls.empty())
		return 0;

	struct invoke_data {
		ControlList *controls;
	} d = {
		.controls = &controls,
	};

	return spa_loop_locked(
		impl->data_loop,
		[](spa_loop *, bool, uint32_t, const void *data, size_t, void *user_data)
		{
			const auto *d = static_cast<const invoke_data *>(data);
			auto *impl = static_cast<struct impl *>(user_data);

			impl->ctrls.merge(std::move(*d->controls),
					  libcamera::ControlList::MergePolicy::OverwriteExisting);

			return 0;
		},
		0, &d, sizeof(d), impl
	);
}

void handle_completed_request(struct impl *impl, libcamera::Request *request)
{
	const auto request_id = request->cookie();
	struct port *port = &impl->out_ports[0];
	buffer *b = &port->buffers[request_id];

	spa_log_trace(impl->log, "%p: request %p[%" PRIu64 "] process status:%u seq:%" PRIu32,
			impl, request, request_id, static_cast<unsigned int>(request->status()),
			request->sequence());

	if (request->status() == libcamera::Request::Status::RequestCancelled) {
		spa_log_trace(impl->log, "%p: request %p[%" PRIu64 "] cancelled",
				impl, request, request_id);
		request->reuse(libcamera::Request::ReuseFlag::ReuseBuffers);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);
		spa_libcamera_buffer_recycle(impl, port, b->id);
		return;
	}

	const FrameBuffer *buffer = request->findBuffer(port->streamConfig.stream());
	if (buffer == nullptr) {
		spa_log_warn(impl->log, "%p: request %p[%" PRIu64 "] has no buffer for stream %p",
				impl, request, request_id, port->streamConfig.stream());
		return;
	}

	const FrameMetadata &fmd = buffer->metadata();

	if (impl->clock) {
		double target = (double)port->info.rate.num / port->info.rate.denom;
		double corr;

		if (impl->dll.bw == 0.0) {
			spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MAX, port->info.rate.denom, port->info.rate.denom);
			impl->clock->next_nsec = fmd.timestamp;
			corr = 1.0;
		} else {
			double diff = ((double)impl->clock->next_nsec - (double)fmd.timestamp) / SPA_NSEC_PER_SEC;
			double error = port->info.rate.denom * (diff - target);
			corr = spa_dll_update(&impl->dll, SPA_CLAMPD(error, -128., 128.));
		}
		/* FIXME, we should follow the driver clock and target_ values.
		 * for now we ignore and use our own. */
		impl->clock->target_rate = port->rate;
		impl->clock->target_duration = 1;

		impl->clock->nsec = fmd.timestamp;
		impl->clock->rate = port->rate;
		impl->clock->position = fmd.sequence;
		impl->clock->duration = 1;
		impl->clock->delay = 0;
		impl->clock->rate_diff = corr;
		impl->clock->next_nsec += (uint64_t) (target * SPA_NSEC_PER_SEC * corr);
	}

	if (b->h) {
		b->h->flags = 0;
		if (fmd.status != libcamera::FrameMetadata::Status::FrameSuccess)
			b->h->flags |= SPA_META_HEADER_FLAG_CORRUPTED;
		b->h->offset = 0;
		b->h->seq = fmd.sequence;
		b->h->pts = fmd.timestamp;
		b->h->dts_offset = 0;
	}

	for (std::size_t i = 0; i < b->outbuf->n_datas; i++) {
		auto *d = &b->outbuf->datas[i];

		d->chunk->flags = 0;

		if (fmd.status != libcamera::FrameMetadata::Status::FrameSuccess)
			d->chunk->flags |= SPA_CHUNK_FLAG_CORRUPTED;
	}

	request->reuse(libcamera::Request::ReuseFlag::ReuseBuffers);

	spa_list_append(&port->queue, &b->link);

	spa_io_buffers *io = port->io;
	if (io == nullptr) {
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
		spa_log_trace(impl->log, "%p: now queued %" PRIu32, impl, b->id);
	}

	spa_node_call_ready(&impl->callbacks, SPA_STATUS_HAVE_DATA);
}

void libcamera_on_fd_events(struct spa_source *source)
{
	struct impl *impl = (struct impl*) source->data;
	uint32_t index;
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

	auto avail = spa_ringbuffer_get_read_index(&impl->completed_requests_rb, &index);
	for (; avail > 0; avail--, index++) {
		auto *request = impl->completed_requests[index & MASK_BUFFERS];

		spa_ringbuffer_read_update(&impl->completed_requests_rb, index + 1);
		handle_completed_request(impl, request);
	}
}

int spa_libcamera_use_buffers(struct impl *impl, struct port *port,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	return -ENOTSUP;
}

const struct {
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

uint32_t libcamera_orientation_to_spa_transform_value(Orientation orientation)
{
	for (const auto& t : orientation_map) {
		if (t.libcamera_orientation == orientation)
			return t.spa_transform_value;
	}
	return SPA_META_TRANSFORMATION_None;
}

int
spa_libcamera_alloc_buffers(struct impl *impl, struct port *port,
		       struct spa_buffer **buffers,
		       uint32_t n_buffers)
{
	if (port->n_buffers > 0)
		return -EIO;

	Stream *stream = impl->config->at(0).stream();
	const std::vector<std::unique_ptr<FrameBuffer>> &bufs =
			impl->allocator.buffers(stream);

	if (n_buffers > 0) {
		if (bufs.size() != n_buffers)
			return -EINVAL;

		spa_data *d = buffers[0]->datas;

		if (d[0].type != SPA_ID_INVALID && d[0].type & (1u << SPA_DATA_DmaBuf)) {
			port->memtype = SPA_DATA_DmaBuf;
		} else if (d[0].type != SPA_ID_INVALID && d[0].type & (1u << SPA_DATA_MemFd)) {
			port->memtype = SPA_DATA_MemFd;
		} else if (d[0].type & (1u << SPA_DATA_MemPtr)) {
			port->memtype = SPA_DATA_MemPtr;
		} else {
			spa_log_error(impl->log, "can't use buffers of type %d", d[0].type);
			return -EINVAL;
		}
	}

	for (uint32_t i = 0; i < n_buffers; i++) {
		struct buffer *b;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(impl->log, "invalid buffer data");
			return -EINVAL;
		}

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = 0;
		b->h = (struct spa_meta_header*)spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		b->videotransform = (struct spa_meta_videotransform*)spa_buffer_find_meta_data(
			buffers[i], SPA_META_VideoTransform, sizeof(*b->videotransform));
		if (b->videotransform) {
			b->videotransform->transform =
				libcamera_orientation_to_spa_transform_value(impl->config->orientation);
			spa_log_debug(impl->log, "Setting videotransform for buffer %u to %u",
				i, b->videotransform->transform);

		}

		spa_data *d = buffers[i]->datas;
		for(uint32_t j = 0; j < buffers[i]->n_datas; ++j) {
			d[j].type = port->memtype;
			d[j].flags = SPA_DATA_FLAG_READABLE;
			d[j].mapoffset = 0;
			d[j].chunk->stride = port->streamConfig.stride;
			d[j].chunk->flags = 0;
			/* Update parameters according to the plane information */
			unsigned int numPlanes = bufs[i]->planes().size();
			if (buffers[i]->n_datas < numPlanes) {
				if (j < buffers[i]->n_datas - 1) {
					d[j].maxsize = bufs[i]->planes()[j].length;
					d[j].chunk->offset = bufs[i]->planes()[j].offset;
					d[j].chunk->size = bufs[i]->planes()[j].length;
				} else {
					d[j].chunk->offset = bufs[i]->planes()[j].offset;
					for (uint8_t k = j; k < numPlanes; k++) {
						d[j].maxsize += bufs[i]->planes()[k].length;
						d[j].chunk->size += bufs[i]->planes()[k].length;
					}
				}
			} else if (buffers[i]->n_datas == numPlanes) {
				d[j].maxsize = bufs[i]->planes()[j].length;
				d[j].chunk->offset = bufs[i]->planes()[j].offset;
				d[j].chunk->size = bufs[i]->planes()[j].length;
			} else {
				spa_log_warn(impl->log, "buffer index: i: %d, data member "
					"numbers: %d is greater than plane number: %d",
					i, buffers[i]->n_datas, numPlanes);
				d[j].maxsize = port->streamConfig.frameSize;
				d[j].chunk->offset = 0;
				d[j].chunk->size = port->streamConfig.frameSize;
			}

			if (port->memtype == SPA_DATA_DmaBuf ||
			    port->memtype == SPA_DATA_MemFd) {
				d[j].flags |= SPA_DATA_FLAG_MAPPABLE;
				d[j].fd = bufs[i]->planes()[j].fd.get();
				spa_log_debug(impl->log, "Got fd = %" PRId64 " for buffer: #%d", d[j].fd, i);
				d[j].data = nullptr;
			}
			else if (port->memtype == SPA_DATA_MemPtr) {
				d[j].fd = -1;
				d[j].data = mmap(nullptr,
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
	}

	port->n_buffers = n_buffers;
	spa_log_debug(impl->log, "we have %d buffers", n_buffers);

	return 0;
}


void impl::requestComplete(libcamera::Request *request)
{
	struct impl *impl = this;
	uint32_t index;

	spa_log_trace(impl->log, "%p: request %p[%" PRIu64 "] completed status:%u seq:%" PRIu32,
			impl, request, request->cookie(),
			static_cast<unsigned int>(request->status()),
			request->sequence());

	spa_ringbuffer_get_write_index(&impl->completed_requests_rb, &index);
	impl->completed_requests[index & MASK_BUFFERS] = request;
	spa_ringbuffer_write_update(&impl->completed_requests_rb, index + 1);

	if (spa_system_eventfd_write(impl->system, impl->source.fd, 1) < 0)
		spa_log_error(impl->log, "Failed to write on event fd");

}

int do_remove_source(struct spa_loop *loop,
		     bool async,
		     uint32_t seq,
		     const void *data,
		     size_t size,
		     void *user_data)
{
	auto *impl = static_cast<struct impl *>(user_data);
	if (impl->source.loop)
		spa_loop_remove_source(loop, &impl->source);
	return 0;
}

int spa_libcamera_stream_on(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	int res;

	if (!port->current_format) {
		spa_log_error(impl->log, "Exiting %s with -EIO", __FUNCTION__);
		return -EIO;
	}

	if (impl->active)
		return 0;

	res = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	if (res < 0)
		return res;

	impl->source.fd = res;
	impl->source.func = libcamera_on_fd_events;
	impl->source.data = impl;
	impl->source.mask = SPA_IO_IN | SPA_IO_ERR;
	impl->source.rmask = 0;
	res = spa_loop_add_source(impl->data_loop, &impl->source);
	if (res < 0)
		goto err_close_source;

	spa_log_info(impl->log, "starting camera %s", impl->camera->id().c_str());
	if ((res = impl->camera->start(&impl->initial_controls)) < 0)
		goto err_remove_source;

	impl->camera->requestCompleted.connect(impl, &impl::requestComplete);

	for (auto& req : impl->requestPool) {
		req->reuse(libcamera::Request::ReuseFlag::ReuseBuffers);

		if ((res = impl->camera->queueRequest(req.get())) < 0)
			goto err_stop_camera;
	}

	impl->dll.bw = 0.0;
	impl->active = true;

	return 0;

err_stop_camera:
	impl->camera->stop();
	impl->camera->requestCompleted.disconnect(impl, &impl::requestComplete);
err_remove_source:
	spa_loop_locked(impl->data_loop, do_remove_source, 0, nullptr, 0, impl);
err_close_source:
	spa_system_close(impl->system, std::exchange(impl->source.fd, -1));

	return res == -EACCES ? -EBUSY : res;
}

int spa_libcamera_stream_off(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	int res;

	if (!impl->active)
		return 0;

	impl->active = false;
	spa_log_info(impl->log, "stopping camera %s", impl->camera->id().c_str());

	if ((res = impl->camera->stop()) < 0) {
		spa_log_warn(impl->log, "error stopping camera %s: %s",
				impl->camera->id().c_str(), spa_strerror(res));
	}

	impl->camera->requestCompleted.disconnect(impl, &impl::requestComplete);

	spa_loop_locked(impl->data_loop, do_remove_source, 0, nullptr, 0, impl);
	if (impl->source.fd >= 0)  {
		spa_system_close(impl->system, impl->source.fd);
		impl->source.fd = -1;
	}

	impl->completed_requests_rb = SPA_RINGBUFFER_INIT();
	spa_list_init(&port->queue);

	return 0;
}

int port_get_format(struct impl *impl, struct port *port,
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

int impl_node_enum_params(void *object, int seq,
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

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
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
		default:
			return spa_libcamera_enum_controls(impl,
					GET_OUT_PORT(impl, 0),
					seq, result.index, 0, num, filter);
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
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

int impl_node_set_param(void *object,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	auto *impl = static_cast<struct impl *>(object);

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		const auto *obj = reinterpret_cast<const spa_pod_object *>(param);
		const struct spa_pod_prop *prop;

		if (param == nullptr)
			return 0;

		libcamera::ControlList controls(impl->camera->controls());
		int res;

		SPA_POD_OBJECT_FOREACH(obj, prop) {
			switch (prop->key) {
			default:
				res = control_list_update_from_prop(controls, prop, SPA_POD_BODY_CONST(&prop->value));
				if (res < 0)
					return res;

				break;
			}
		}

		res = spa_libcamera_apply_controls(impl, std::move(controls));
		if (res < 0)
			return res;

		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		impl->clock = (struct spa_io_clock*)data;
		if (impl->clock)
			SPA_FLAG_SET(impl->clock->flags, SPA_IO_CLOCK_FLAG_NO_RATE);
		break;
	case SPA_IO_Position:
		impl->position = (struct spa_io_position*)data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *impl = (struct impl*)object;
	int res;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
	spa_return_val_if_fail(command != nullptr, -EINVAL);

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

void emit_node_info(struct impl *impl, bool full)
{
	static const struct spa_dict_item info_items[] = {
		{ SPA_KEY_DEVICE_API, "libcamera" },
		{ SPA_KEY_MEDIA_CLASS, "Video/Source" },
		{ SPA_KEY_MEDIA_ROLE, "Camera" },
		{ SPA_KEY_NODE_DRIVER, "true" },
	};
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

void emit_port_info(struct impl *impl, struct port *port, bool full)
{
	static const struct spa_dict_item info_items[] = {
		{ SPA_KEY_PORT_GROUP, "stream.0" },
	};
	uint64_t old = full ? port->info.change_mask : 0;
	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict dict = SPA_DICT_INIT_ARRAY(info_items);
		port->info.props = &dict;
		spa_node_emit_port_info(&impl->hooks,
				SPA_DIRECTION_OUTPUT, 0, &port->info);
		port->info.change_mask = old;
	}
}

int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *impl = (struct impl*)object;
	struct spa_hook_list save;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);

	emit_node_info(impl, true);
	emit_port_info(impl, GET_OUT_PORT(impl, 0), true);

	spa_hook_list_join(&impl->hooks, &save);

	return 0;
}

int impl_node_set_callbacks(void *object,
			    const struct spa_node_callbacks *callbacks,
			    void *data)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	impl->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

int impl_node_sync(void *object, int seq)
{
	struct impl *impl = (struct impl*)object;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	spa_node_emit_result(&impl->hooks, seq, 0, 0, nullptr);

	return 0;
}

int impl_node_add_port(void *object,
		       enum spa_direction direction,
		       uint32_t port_id, const struct spa_dict *props)
{
	return -ENOTSUP;
}

int impl_node_remove_port(void *object,
		          enum spa_direction direction,
			  uint32_t port_id)
{
	return -ENOTSUP;
}

int impl_node_port_enum_params(void *object, int seq,
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

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
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
		return spa_libcamera_enum_controls(impl, port, seq, start, 0, num, filter);

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
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->buffers_blocks),
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
	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
			param = spa_latency_build(&b, id, &impl->latency[result.index]);
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

int port_set_format(struct impl *impl, struct port *port,
		    uint32_t flags, const struct spa_pod *format)
{
	const bool try_only = SPA_FLAG_IS_SET(flags, SPA_NODE_PARAM_FLAG_TEST_ONLY);

	if (!try_only) {
		spa_libcamera_stream_off(impl);
		spa_libcamera_clear_buffers(impl, port);
		freeBuffers(impl, port);
		port->current_format.reset();
	}

	if (format == nullptr) {
		if (!try_only)
			spa_libcamera_close(impl);
	} else {
		spa_video_info info;
		int res;

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

			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			if (spa_format_video_mjpg_parse(format, &info.info.mjpg) < 0)
				return -EINVAL;

			break;
		case SPA_MEDIA_SUBTYPE_h264:
			if (spa_format_video_h264_parse(format, &info.info.h264) < 0)
				return -EINVAL;

			break;
		default:
			return -EINVAL;
		}

		res = spa_libcamera_set_format(impl, port, &info, try_only);
		if (res < 0)
			return res;

		if (!try_only)
			port->current_format = info;
	}

	if (try_only)
		return 0;

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

int impl_node_port_set_param(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t id, uint32_t flags,
			     const struct spa_pod *param)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(object != nullptr, -EINVAL);
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

int impl_node_port_use_buffers(void *object,
			       enum spa_direction direction,
			       uint32_t port_id,
			       uint32_t flags,
			       struct spa_buffer **buffers,
			       uint32_t n_buffers)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
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
	if (buffers == nullptr)
		return 0;

	if (flags & SPA_NODE_BUFFERS_FLAG_ALLOC) {
		res = spa_libcamera_alloc_buffers(impl, port, buffers, n_buffers);
	} else {
		res = spa_libcamera_use_buffers(impl, port, buffers, n_buffers);
	}
	return res;
}

int impl_node_port_set_io(void *object,
			  enum spa_direction direction,
			  uint32_t port_id,
			  uint32_t id,
			  void *data, size_t size)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(impl, direction, port_id), -EINVAL);

	port = GET_PORT(impl, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = (struct spa_io_buffers*)data;
		break;
	case SPA_IO_Control:
		port->control = (struct spa_io_sequence*)data;
		port->control_size = size;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

int impl_node_port_reuse_buffer(void *object,
				uint32_t port_id,
				uint32_t buffer_id)
{
	struct impl *impl = (struct impl*)object;
	struct port *port;
	int res;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);
	spa_return_val_if_fail(port_id == 0, -EINVAL);

	port = GET_OUT_PORT(impl, port_id);

	spa_return_val_if_fail(buffer_id < port->n_buffers, -EINVAL);

	res = spa_libcamera_buffer_recycle(impl, port, buffer_id);

	return res;
}

int process_control(struct impl *impl, struct spa_pod_sequence *control, uint32_t size)
{
	libcamera::ControlList controls(impl->camera->controls());
	struct spa_pod_parser parser[2];
	struct spa_pod_frame frame[2];
	struct spa_pod_sequence seq;
	const void *seq_body, *c_body;
	struct spa_pod_control c;
	int res;

	spa_pod_parser_init_from_data(&parser[0], control, size, 0, size);
	if (spa_pod_parser_push_sequence_body(&parser[0], &frame[0], &seq, &seq_body) < 0)
		return 0;

	while (spa_pod_parser_get_control_body(&parser[0], &c, &c_body) >= 0) {
		switch (c.type) {
		case SPA_CONTROL_Properties: {
			struct spa_pod_object obj;
			struct spa_pod_prop prop;
			const void *obj_body, *prop_body;

			if (spa_pod_parser_init_object_body(&parser[1], &frame[1],
					&c.value, c_body, &obj, &obj_body) < 0)
				continue;
			while (spa_pod_parser_get_prop_body(&parser[1], &prop, &prop_body) >= 0) {
				res = control_list_update_from_prop(controls, &prop, prop_body);
				if (res < 0)
					return res;
			}

			break;
		}
		default:
			break;
		}
	}

	res = spa_libcamera_apply_controls(impl, std::move(controls));
	if (res < 0)
		return res;

	return 0;
}

int impl_node_process(void *object)
{
	struct impl *impl = (struct impl*)object;
	int res;
	struct spa_io_buffers *io;
	struct port *port;
	struct buffer *b;

	spa_return_val_if_fail(impl != nullptr, -EINVAL);

	port = GET_OUT_PORT(impl, 0);
	if ((io = port->io) == nullptr)
		return -EIO;

	if (port->control)
		process_control(impl, &port->control->sequence, port->control_size);

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

const struct spa_node_methods impl_node = {
	.version = SPA_VERSION_NODE_METHODS,
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

int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto *impl = reinterpret_cast<struct impl *>(handle);

	spa_return_val_if_fail(handle != nullptr, -EINVAL);
	spa_return_val_if_fail(interface != nullptr, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &impl->node;
	else
		return -ENOENT;

	return 0;
}

int impl_clear(struct spa_handle *handle)
{
	std::destroy_at(reinterpret_cast<impl *>(handle));
	return 0;
}

impl::impl(spa_log *log, spa_loop *data_loop, spa_system *system,
	   std::shared_ptr<CameraManager> manager, std::shared_ptr<Camera> camera,
	   std::unique_ptr<CameraConfiguration> config)
	: handle({ SPA_VERSION_HANDLE, impl_get_interface, impl_clear }),
	  log(log),
	  data_loop(data_loop),
	  system(system),
	  out_ports{{this}},
	  manager(std::move(manager)),
	  camera(std::move(camera)),
	  config(std::move(config)),
	  allocator(this->camera)
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

	latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);
}

size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	int res;

	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(handle != nullptr, -EINVAL);

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

	const char *device_id = info
		? spa_dict_lookup(info, SPA_KEY_API_LIBCAMERA_PATH)
		: nullptr;

	auto camera = device_id ? manager->get(device_id) : nullptr;
	if (!camera) {
		spa_log_error(log, "unknown camera id: %s", device_id);
		return -ENOENT;
	}

	auto config = camera->generateConfiguration({ libcamera::StreamRole::VideoRecording });
	if (!config) {
		spa_log_error(log, "cannot generate configuration for camera");
		return -EINVAL;
	}

	new (handle) impl(log, data_loop, system,
			  std::move(manager), std::move(camera),
			  std::move(config));

	return 0;
}

const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

int impl_enum_interface_info(const struct spa_handle_factory *factory,
			     const struct spa_interface_info **info,
			     uint32_t *index)
{
	spa_return_val_if_fail(factory != nullptr, -EINVAL);
	spa_return_val_if_fail(info != nullptr, -EINVAL);
	spa_return_val_if_fail(index != nullptr, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

}

extern "C" {
const struct spa_handle_factory spa_libcamera_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_LIBCAMERA_SOURCE,
	nullptr,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
}
