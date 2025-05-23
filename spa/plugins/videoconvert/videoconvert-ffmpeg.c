/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/mman.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>

#include <spa/support/plugin.h>
#include <spa/support/cpu.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/ratelimit.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/types.h>
#include <spa/debug/format.h>
#include <spa/control/ump-utils.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.videoconvert.ffmpeg");

#define MAX_ALIGN	64u
#define MAX_BUFFERS	32
#define MAX_DATAS	4
#define MAX_PORTS	(1+1)

struct props {
	unsigned int dummy:1;
};

static void props_reset(struct props *props)
{
	props->dummy = false;
}

struct buffer {
	uint32_t id;
#define BUFFER_FLAG_QUEUED	(1<<0)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
	void *datas[MAX_DATAS];
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define IDX_Tag		6
#define N_PORT_PARAMS	7
	struct spa_param_info params[N_PORT_PARAMS];

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_latency_info latency[2];
	unsigned int have_latency:1;

	struct spa_video_info format;
	unsigned int valid:1;
	unsigned int have_format:1;
	unsigned int is_dsp:1;
	unsigned int is_monitor:1;
	unsigned int is_control:1;

	uint32_t blocks;
	uint32_t stride;
	uint32_t maxsize;

	struct spa_list queue;
};

struct dir {
	struct port *ports[MAX_PORTS];
	uint32_t n_ports;

	enum spa_direction direction;
	enum spa_param_port_config_mode mode;

	struct spa_video_info format;
	unsigned int have_format:1;
	unsigned int have_profile:1;
	struct spa_pod *tag;
	enum AVPixelFormat pix_fmt;
	int width;
	int height;

	unsigned int control:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;
	struct spa_loop *data_loop;

	uint32_t cpu_flags;
	uint32_t max_align;
	uint32_t quantum_limit;
	enum spa_direction direction;

	struct spa_ratelimit rate_limit;

	struct props props;

	struct spa_io_position *io_position;
	struct spa_io_rate_match *io_rate_match;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_EnumPortConfig	0
#define IDX_PortConfig		1
#define IDX_PropInfo		2
#define IDX_Props		3
#define N_NODE_PARAMS		4
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_hook_list hooks;

	unsigned int monitor:1;

	struct dir dir[2];

	unsigned int started:1;
	unsigned int setup:1;
	unsigned int fmt_passthrough:1;
	unsigned int drained:1;
	unsigned int port_ignore_latency:1;
	unsigned int monitor_passthrough:1;

	char group_name[128];

	struct {
		const AVCodec *codec;
		AVCodecContext *context;
		AVPacket *packet;
		AVFrame *frame;
	} decoder;
	struct {
		struct SwsContext *context;
		AVFrame *frame;
	} convert;
	struct {
		const AVCodec *codec;
		AVCodecContext *context;
		AVFrame *frame;
		AVPacket *packet;
	} encoder;
};

#define CHECK_PORT(this,d,p)		((p) < this->dir[d].n_ports)
#define GET_PORT(this,d,p)		(this->dir[d].ports[p])
#define GET_IN_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_INPUT,p)
#define GET_OUT_PORT(this,p)		GET_PORT(this,SPA_DIRECTION_OUTPUT,p)

#define PORT_IS_DSP(this,d,p)		(GET_PORT(this,d,p)->is_dsp)
#define PORT_IS_CONTROL(this,d,p)	(GET_PORT(this,d,p)->is_control)

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			SPA_FOR_EACH_ELEMENT_VAR(this->params, p) {
				if (p->user > 0) {
					p->flags ^= SPA_PARAM_INFO_SERIAL;
					p->user = 0;
				}
			}
		}
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
		struct spa_dict_item items[5];
		uint32_t n_items = 0;

		if (PORT_IS_DSP(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float video");
			if (port->is_monitor)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_MONITOR, "true");
			if (this->port_ignore_latency)
				items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_IGNORE_LATENCY, "true");
		} else if (PORT_IS_CONTROL(this, port->direction, port->id)) {
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "control");
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "8 bit raw midi");
		}
		if (this->group_name[0] != '\0')
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_GROUP, this->group_name);
		port->info.props = &SPA_DICT_INIT(items, n_items);

		if (port->info.change_mask & SPA_PORT_CHANGE_MASK_PARAMS) {
			SPA_FOR_EACH_ELEMENT_VAR(port->params, p) {
				if (p->user > 0) {
					p->flags ^= SPA_PARAM_INFO_SERIAL;
					p->user = 0;
				}
			}
		}
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id,
		bool is_dsp, bool is_monitor, bool is_control)
{
	struct port *port = GET_PORT(this, direction, port_id);

	spa_assert(port_id < MAX_PORTS);

	if (port == NULL) {
		port = calloc(1, sizeof(struct port));
		if (port == NULL)
			return -errno;
		this->dir[direction].ports[port_id] = port;
	}
	port->direction = direction;
	port->id = port_id;
	port->latency[SPA_DIRECTION_INPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_INPUT);
	port->latency[SPA_DIRECTION_OUTPUT] = SPA_LATENCY_INFO(SPA_DIRECTION_OUTPUT);

	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
		SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->params[IDX_Tag] = SPA_PARAM_INFO(SPA_PARAM_Tag, SPA_PARAM_INFO_READWRITE);
	port->info.params = port->params;
	port->info.n_params = N_PORT_PARAMS;

	port->n_buffers = 0;
	port->have_format = false;
	port->is_monitor = is_monitor;
	port->is_dsp = is_dsp;
	if (port->is_dsp) {
		port->format.media_type = SPA_MEDIA_TYPE_video;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_dsp;
		port->format.info.dsp.format = SPA_VIDEO_FORMAT_DSP_F32;
		port->blocks = 1;
		port->stride = 16;
	}
	port->is_control = is_control;
	if (port->is_control) {
		port->format.media_type = SPA_MEDIA_TYPE_application;
		port->format.media_subtype = SPA_MEDIA_SUBTYPE_control;
		port->blocks = 1;
		port->stride = 1;
	}
	port->valid = true;
	spa_list_init(&port->queue);

	spa_log_debug(this->log, "%p: add port %d:%d %d %d %d",
			this, direction, port_id, is_dsp, is_monitor, is_control);
	emit_port_info(this, port, true);

	return 0;
}

static int deinit_port(struct impl *this, enum spa_direction direction, uint32_t port_id)
{
	struct port *port = GET_PORT(this, direction, port_id);
	if (port == NULL || !port->valid)
		return -ENOENT;
	port->valid = false;
	spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);
	return 0;
}

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
	{
		struct dir *dir;
		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, id,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_CHOICE_ENUM_Id(4,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_dsp,
				SPA_PARAM_PORT_CONFIG_MODE_convert),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_CHOICE_Bool(false),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_CHOICE_Bool(false));
		break;
	}
	case SPA_PARAM_PortConfig:
	{
		struct dir *dir;
		struct spa_pod_frame f[1];

		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamPortConfig, id);
		spa_pod_builder_add(&b,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(dir->mode),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(this->monitor),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(dir->control),
			0);

		if (dir->have_format) {
			spa_pod_builder_prop(&b, SPA_PARAM_PORT_CONFIG_format, 0);
			spa_format_video_build(&b, SPA_PARAM_PORT_CONFIG_format,
					&dir->format);
		}
		param = spa_pod_builder_pop(&b, &f[0]);
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
		default:
			return 0;
		}
		break;
	}

	case SPA_PARAM_Props:
	{
		struct spa_pod_frame f[2];

		switch (result.index) {
		case 0:
			spa_pod_builder_push_object(&b, &f[0],
                                SPA_TYPE_OBJECT_Props, id);
			param = spa_pod_builder_pop(&b, &f[0]);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return 0;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: io %d %p/%zd", this, id, data, size);

	switch (id) {
	case SPA_IO_Position:
		this->io_position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int videoconvert_set_param(struct impl *this, const char *k, const char *s)
{
	return 0;
}

static int parse_prop_params(struct impl *this, struct spa_pod *params)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f;
	int changed = 0;

	spa_pod_parser_pod(&prs, params);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return 0;

	while (true) {
		const char *name;
		struct spa_pod *pod;
		char value[512];

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;

		if (spa_pod_parser_get_pod(&prs, &pod) < 0)
			break;

		if (spa_pod_is_string(pod)) {
			spa_pod_copy_string(pod, sizeof(value), value);
		} else if (spa_pod_is_float(pod)) {
			spa_dtoa(value, sizeof(value),
					SPA_POD_VALUE(struct spa_pod_float, pod));
		} else if (spa_pod_is_double(pod)) {
			spa_dtoa(value, sizeof(value),
					SPA_POD_VALUE(struct spa_pod_double, pod));
		} else if (spa_pod_is_int(pod)) {
			snprintf(value, sizeof(value), "%d",
					SPA_POD_VALUE(struct spa_pod_int, pod));
		} else if (spa_pod_is_bool(pod)) {
			snprintf(value, sizeof(value), "%s",
					SPA_POD_VALUE(struct spa_pod_bool, pod) ?
					"true" : "false");
		} else if (spa_pod_is_none(pod)) {
			spa_zero(value);
		} else
			continue;

		spa_log_info(this->log, "key:'%s' val:'%s'", name, value);
		changed += videoconvert_set_param(this, name, value);
	}
	return changed;
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	int changed = 0;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_params:
			changed += parse_prop_params(this, &prop->value);
			break;
		default:
			break;
		}
	}
	return changed;
}

static int reconfigure_mode(struct impl *this, enum spa_param_port_config_mode mode,
		enum spa_direction direction, bool monitor, bool control, struct spa_video_info *info)
{
	struct dir *dir;
	uint32_t i;

	dir = &this->dir[direction];

	if (dir->have_profile && this->monitor == monitor && dir->mode == mode &&
	    dir->control == control &&
	    (info == NULL || memcmp(&dir->format, info, sizeof(*info)) == 0))
		return 0;

	spa_log_debug(this->log, "%p: port config direction:%d monitor:%d "
			"control:%d mode:%d %d", this, direction, monitor,
			control, mode, dir->n_ports);

	for (i = 0; i < dir->n_ports; i++) {
		deinit_port(this, direction, i);
		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			deinit_port(this, SPA_DIRECTION_OUTPUT, i+1);
	}

	this->monitor = monitor;
	this->setup = false;
	dir->control = control;
	dir->have_profile = true;
	dir->mode = mode;

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_dsp:
	{
		if (info) {
			dir->n_ports = 1;
			dir->format = *info;
			dir->format.info.dsp.format = SPA_VIDEO_FORMAT_DSP_F32;
			dir->have_format = true;
		} else {
			dir->n_ports = 0;
		}

		if (this->monitor && direction == SPA_DIRECTION_INPUT)
			this->dir[SPA_DIRECTION_OUTPUT].n_ports = dir->n_ports + 1;

		for (i = 0; i < dir->n_ports; i++) {
			init_port(this, direction, i, true, false, false);
			if (this->monitor && direction == SPA_DIRECTION_INPUT)
				init_port(this, SPA_DIRECTION_OUTPUT, i+1, true, true, false);
		}
		break;
	}
	case SPA_PARAM_PORT_CONFIG_MODE_convert:
	{
		dir->n_ports = 1;
		dir->have_format = false;
		init_port(this, direction, 0, false, false, false);
		break;
	}
	case SPA_PARAM_PORT_CONFIG_MODE_none:
		break;
	default:
		return -ENOTSUP;
	}
	if (direction == SPA_DIRECTION_INPUT && dir->control) {
		i = dir->n_ports++;
		init_port(this, direction, i, false, false, true);
	}
	/* when output is convert mode, we are in OUTPUT (merge) mode, we always output all
	 * the incoming data to output. When output is DSP, we need to output quantum size
	 * chunks. */
	this->direction = this->dir[SPA_DIRECTION_OUTPUT].mode == SPA_PARAM_PORT_CONFIG_MODE_convert ?
		SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_Props].user++;
	this->params[IDX_PortConfig].user++;
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (param == NULL)
		return 0;

	switch (id) {
	case SPA_PARAM_PortConfig:
	{
		struct spa_video_info info = { 0, }, *infop = NULL;
		struct spa_pod *format = NULL;
		enum spa_direction direction;
		enum spa_param_port_config_mode mode;
		bool monitor = false, control = false;
		int res;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&direction),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_OPT_Bool(&monitor),
				SPA_PARAM_PORT_CONFIG_control,		SPA_POD_OPT_Bool(&control),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_OPT_Pod(&format)) < 0)
			return -EINVAL;

		if (format) {
			if (!spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format))
				return -EINVAL;

			if ((res = spa_format_video_parse(format, &info)) < 0)
				return res;

			infop = &info;
		}

		if ((res = reconfigure_mode(this, mode, direction, monitor, control, infop)) < 0)
			return res;

		emit_node_info(this, false);
		break;
	}
	case SPA_PARAM_Props:
		if (apply_props(this, param) > 0)
			emit_node_info(this, false);
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static enum AVPixelFormat format_to_pix_fmt(uint32_t format)
{
	switch (format) {
	case SPA_VIDEO_FORMAT_I420:
		return AV_PIX_FMT_YUV420P;
	case SPA_VIDEO_FORMAT_YV12:
		break;
	case SPA_VIDEO_FORMAT_YUY2:
		return AV_PIX_FMT_YUYV422;
	case SPA_VIDEO_FORMAT_UYVY:
		return AV_PIX_FMT_UYVY422;
	case SPA_VIDEO_FORMAT_AYUV:
		break;
	case SPA_VIDEO_FORMAT_RGBx:
		return AV_PIX_FMT_RGB0;
	case SPA_VIDEO_FORMAT_BGRx:
		return AV_PIX_FMT_BGR0;
	case SPA_VIDEO_FORMAT_xRGB:
		return AV_PIX_FMT_0RGB;
	case SPA_VIDEO_FORMAT_xBGR:
		return AV_PIX_FMT_0BGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return AV_PIX_FMT_RGBA;
	case SPA_VIDEO_FORMAT_BGRA:
		return AV_PIX_FMT_BGRA;
	case SPA_VIDEO_FORMAT_ARGB:
		return AV_PIX_FMT_ARGB;
	case SPA_VIDEO_FORMAT_ABGR:
		return AV_PIX_FMT_ABGR;
	case SPA_VIDEO_FORMAT_RGB:
		return AV_PIX_FMT_RGB24;
	case SPA_VIDEO_FORMAT_BGR:
		return AV_PIX_FMT_BGR24;
	case SPA_VIDEO_FORMAT_Y41B:
		return AV_PIX_FMT_YUV411P;
	case SPA_VIDEO_FORMAT_Y42B:
		return AV_PIX_FMT_YUV422P;
	case SPA_VIDEO_FORMAT_YVYU:
		return AV_PIX_FMT_YVYU422;
	case SPA_VIDEO_FORMAT_Y444:
		return AV_PIX_FMT_YUV444P;
	case SPA_VIDEO_FORMAT_v210:
	case SPA_VIDEO_FORMAT_v216:
		break;
	case SPA_VIDEO_FORMAT_NV12:
		return AV_PIX_FMT_NV12;
	case SPA_VIDEO_FORMAT_NV21:
		return AV_PIX_FMT_NV21;
	case SPA_VIDEO_FORMAT_GRAY8:
		return AV_PIX_FMT_GRAY8;
	case SPA_VIDEO_FORMAT_GRAY16_BE:
		return AV_PIX_FMT_GRAY16BE;
	case SPA_VIDEO_FORMAT_GRAY16_LE:
		return AV_PIX_FMT_GRAY16LE;
	case SPA_VIDEO_FORMAT_v308:
		break;
	case SPA_VIDEO_FORMAT_RGB16:
		return AV_PIX_FMT_RGB565;
	case SPA_VIDEO_FORMAT_BGR16:
		break;
	case SPA_VIDEO_FORMAT_RGB15:
		return AV_PIX_FMT_RGB555;
	case SPA_VIDEO_FORMAT_BGR15:
	case SPA_VIDEO_FORMAT_UYVP:
		break;
	case SPA_VIDEO_FORMAT_A420:
		return AV_PIX_FMT_YUVA420P;
	case SPA_VIDEO_FORMAT_RGB8P:
		return AV_PIX_FMT_PAL8;
	case SPA_VIDEO_FORMAT_YUV9:
		return AV_PIX_FMT_YUV410P;
	case SPA_VIDEO_FORMAT_YVU9:
	case SPA_VIDEO_FORMAT_IYU1:
	case SPA_VIDEO_FORMAT_ARGB64:
	case SPA_VIDEO_FORMAT_AYUV64:
	case SPA_VIDEO_FORMAT_r210:
		break;
	case SPA_VIDEO_FORMAT_I420_10BE:
		return AV_PIX_FMT_YUV420P10BE;
	case SPA_VIDEO_FORMAT_I420_10LE:
		return AV_PIX_FMT_YUV420P10LE;
	case SPA_VIDEO_FORMAT_I422_10BE:
		return AV_PIX_FMT_YUV422P10BE;
	case SPA_VIDEO_FORMAT_I422_10LE:
		return AV_PIX_FMT_YUV422P10LE;
	case SPA_VIDEO_FORMAT_Y444_10BE:
		return AV_PIX_FMT_YUV444P10BE;
	case SPA_VIDEO_FORMAT_Y444_10LE:
		return AV_PIX_FMT_YUV444P10LE;
	case SPA_VIDEO_FORMAT_GBR:
		return AV_PIX_FMT_GBRP;
	case SPA_VIDEO_FORMAT_GBR_10BE:
		return AV_PIX_FMT_GBRP10BE;
	case SPA_VIDEO_FORMAT_GBR_10LE:
		return AV_PIX_FMT_GBRP10LE;
	case SPA_VIDEO_FORMAT_NV16:
	case SPA_VIDEO_FORMAT_NV24:
	case SPA_VIDEO_FORMAT_NV12_64Z32:
		break;
	case SPA_VIDEO_FORMAT_A420_10BE:
		return AV_PIX_FMT_YUVA420P10BE;
	case SPA_VIDEO_FORMAT_A420_10LE:
		return AV_PIX_FMT_YUVA420P10LE;
	case SPA_VIDEO_FORMAT_A422_10BE:
		return AV_PIX_FMT_YUVA422P10BE;
	case SPA_VIDEO_FORMAT_A422_10LE:
		return AV_PIX_FMT_YUVA422P10LE;
	case SPA_VIDEO_FORMAT_A444_10BE:
		return AV_PIX_FMT_YUVA444P10BE;
	case SPA_VIDEO_FORMAT_A444_10LE:
		return AV_PIX_FMT_YUVA444P10LE;
	case SPA_VIDEO_FORMAT_NV61:
	case SPA_VIDEO_FORMAT_P010_10BE:
	case SPA_VIDEO_FORMAT_P010_10LE:
	case SPA_VIDEO_FORMAT_IYU2:
	case SPA_VIDEO_FORMAT_VYUY:
		break;
	case SPA_VIDEO_FORMAT_GBRA:
		return AV_PIX_FMT_GBRAP;
	case SPA_VIDEO_FORMAT_GBRA_10BE:
		return AV_PIX_FMT_GBRAP10BE;
	case SPA_VIDEO_FORMAT_GBRA_10LE:
		return AV_PIX_FMT_GBRAP10LE;
	case SPA_VIDEO_FORMAT_GBR_12BE:
		return AV_PIX_FMT_GBRP12BE;
	case SPA_VIDEO_FORMAT_GBR_12LE:
		return AV_PIX_FMT_GBRP12LE;
	case SPA_VIDEO_FORMAT_GBRA_12BE:
		return AV_PIX_FMT_GBRAP12BE;
	case SPA_VIDEO_FORMAT_GBRA_12LE:
		return AV_PIX_FMT_GBRAP12LE;
	case SPA_VIDEO_FORMAT_I420_12BE:
		return AV_PIX_FMT_YUV420P12BE;
	case SPA_VIDEO_FORMAT_I420_12LE:
		return AV_PIX_FMT_YUV420P12LE;
	case SPA_VIDEO_FORMAT_I422_12BE:
		return AV_PIX_FMT_YUV422P12BE;
	case SPA_VIDEO_FORMAT_I422_12LE:
		return AV_PIX_FMT_YUV422P12LE;
	case SPA_VIDEO_FORMAT_Y444_12BE:
		return AV_PIX_FMT_YUV444P12BE;
	case SPA_VIDEO_FORMAT_Y444_12LE:
		return AV_PIX_FMT_YUV444P12LE;

	case SPA_VIDEO_FORMAT_RGBA_F16:
	case SPA_VIDEO_FORMAT_RGBA_F32:
		break;

	case SPA_VIDEO_FORMAT_xRGB_210LE:
		return AV_PIX_FMT_X2RGB10LE;
	case SPA_VIDEO_FORMAT_xBGR_210LE:
		return AV_PIX_FMT_X2BGR10LE;

	case SPA_VIDEO_FORMAT_RGBx_102LE:
	case SPA_VIDEO_FORMAT_BGRx_102LE:
	case SPA_VIDEO_FORMAT_ARGB_210LE:
	case SPA_VIDEO_FORMAT_ABGR_210LE:
	case SPA_VIDEO_FORMAT_RGBA_102LE:
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		break;
	default:
		break;
	}
	return AV_PIX_FMT_NONE;
}

static int get_format(struct dir *dir, int *width, int *height, uint32_t *format)
{
	if (dir->have_format) {
		switch (dir->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			*width = dir->format.info.raw.size.width;
			*height = dir->format.info.raw.size.height;
			*format = dir->format.info.raw.format;
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			*width = dir->format.info.mjpg.size.width;
			*height = dir->format.info.mjpg.size.height;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			*width = dir->format.info.h264.size.width;
			*height = dir->format.info.h264.size.height;
			break;
		default:
			*width = *height = 0;
			break;
		}
	} else {
		*width = *height = 0;
	}
	return 0;
}


static int setup_convert(struct impl *this)
{
	struct dir *in, *out;
	uint32_t format;

	in = &this->dir[SPA_DIRECTION_INPUT];
	out = &this->dir[SPA_DIRECTION_OUTPUT];

	spa_log_debug(this->log, "%p: setup:%d in_format:%d out_format:%d", this,
			this->setup, in->have_format, out->have_format);

	if (this->setup)
		return 0;

	if (!in->have_format || !out->have_format)
		return -EIO;

	switch (in->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		in->pix_fmt = format_to_pix_fmt(in->format.info.raw.format);
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			out->pix_fmt = format_to_pix_fmt(out->format.info.raw.format);
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			if ((this->encoder.codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG)) == NULL) {
				spa_log_error(this->log, "failed to find MJPEG encoder");
				return -ENOTSUP;
			}
			out->format.media_subtype = SPA_MEDIA_SUBTYPE_raw;
			out->format.info.raw.format = SPA_VIDEO_FORMAT_I420;
			out->format.info.raw.size = in->format.info.raw.size;
			out->pix_fmt = AV_PIX_FMT_YUVJ420P;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			if ((this->encoder.codec = avcodec_find_encoder(AV_CODEC_ID_H264)) == NULL) {
				spa_log_error(this->log, "failed to find H264 encoder");
				return -ENOTSUP;
			}
			break;
		default:
			return -ENOTSUP;
		}
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_mjpg:
			/* passthrough */
			break;
		case SPA_MEDIA_SUBTYPE_raw:
			out->pix_fmt = format_to_pix_fmt(out->format.info.raw.format);
			if ((this->decoder.codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG)) == NULL) {
				spa_log_error(this->log, "failed to find MJPEG decoder");
				return -ENOTSUP;
			}
			break;
		default:
			return -ENOTSUP;
		}
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_h264:
			/* passthrough */
			break;
		case SPA_MEDIA_SUBTYPE_raw:
			out->pix_fmt = format_to_pix_fmt(out->format.info.raw.format);
			if ((this->decoder.codec = avcodec_find_decoder(AV_CODEC_ID_H264)) == NULL) {
				spa_log_error(this->log, "failed to find H264 decoder");
				return -ENOTSUP;
			}
			break;
		default:
			return -ENOTSUP;
		}
		break;
	default:
		return -ENOTSUP;
	}

	get_format(in, &in->width, &in->height, &format);
	get_format(out, &out->width, &out->height, &format);

	if (this->decoder.codec) {
		if ((this->decoder.context = avcodec_alloc_context3(this->decoder.codec)) == NULL)
			return -EIO;

		if ((this->decoder.packet = av_packet_alloc()) == NULL)
			return -EIO;

		this->decoder.context->flags2 |= AV_CODEC_FLAG2_FAST;

		if (avcodec_open2(this->decoder.context, this->decoder.codec, NULL) < 0) {
			spa_log_error(this->log, "failed to open decoder codec");
			return -EIO;
		}
	}
	if ((this->decoder.frame = av_frame_alloc()) == NULL)
		return -EIO;
	if (this->encoder.codec) {
		if ((this->encoder.context = avcodec_alloc_context3(this->encoder.codec)) == NULL)
			return -EIO;

		if ((this->encoder.packet = av_packet_alloc()) == NULL)
			return -EIO;
		if ((this->encoder.frame = av_frame_alloc()) == NULL)
			return -EIO;

		this->encoder.context->flags2 |= AV_CODEC_FLAG2_FAST;
		this->encoder.context->time_base.num = 1;
		this->encoder.context->width = out->width;
		this->encoder.context->height = out->height;
		this->encoder.context->pix_fmt = out->pix_fmt;

		if (avcodec_open2(this->encoder.context, this->encoder.codec, NULL) < 0) {
			spa_log_error(this->log, "failed to open encoder codec");
			return -EIO;
		}
	}
	if ((this->convert.frame = av_frame_alloc()) == NULL)
		return -EIO;


	this->setup = true;

	emit_node_info(this, false);

	return 0;
}

static void reset_node(struct impl *this)
{
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (this->started)
			return 0;
		if ((res = setup_convert(this)) < 0)
			return res;
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Suspend:
		this->setup = false;
		SPA_FALLTHROUGH;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	case SPA_NODE_COMMAND_Flush:
		reset_node(this);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	uint32_t i;
	struct spa_hook_list save;
	struct port *p;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	for (i = 0; i < this->dir[SPA_DIRECTION_INPUT].n_ports; i++) {
		if ((p = GET_IN_PORT(this, i)) && p->valid)
			emit_port_info(this, p, true);
	}
	for (i = 0; i < this->dir[SPA_DIRECTION_OUTPUT].n_ports; i++) {
		if ((p = GET_OUT_PORT(this, i)) && p->valid)
			emit_port_info(this, p, true);
	}
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

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = object;
	struct dir *other = &this->dir[SPA_DIRECTION_REVERSE(direction)];
	struct spa_pod_frame f[1];
	int width, height;
	uint32_t format = 0;

	get_format(other, &width, &height, &format);

	switch (index) {
	case 0:
		if (PORT_IS_DSP(this, direction, port_id)) {
			struct spa_video_info_dsp info;
			info.format = SPA_VIDEO_FORMAT_DSP_F32;
			*param = spa_format_video_dsp_build(builder,
				SPA_PARAM_EnumFormat, &info);
		} else if (PORT_IS_CONTROL(this, direction, port_id)) {
			*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types,  SPA_POD_CHOICE_FLAGS_Int(
					(1u<<SPA_CONTROL_UMP) | (1u<<SPA_CONTROL_Properties)));
		} else {
			if (other->have_format) {
				*param = spa_format_video_build(builder, SPA_PARAM_EnumFormat, &other->format);
			} else {
				*param = NULL;
			}
		}
		break;
	case 1:
		if (PORT_IS_DSP(this, direction, port_id) ||
		    PORT_IS_CONTROL(this, direction, port_id))
			return 0;

		spa_pod_builder_push_object(builder, &f[0],
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,   SPA_POD_CHOICE_ENUM_Id(7,
						format,
						SPA_VIDEO_FORMAT_YUY2,
						SPA_VIDEO_FORMAT_I420,
						SPA_VIDEO_FORMAT_UYVY,
						SPA_VIDEO_FORMAT_YVYU,
						SPA_VIDEO_FORMAT_RGBA,
						SPA_VIDEO_FORMAT_BGRx),
			0);
		if (width != 0 && height != 0) {
			spa_pod_builder_add(builder,
				SPA_FORMAT_VIDEO_size,     SPA_POD_CHOICE_RANGE_Rectangle(
					&SPA_RECTANGLE(width, height),
					&SPA_RECTANGLE(1, 1),
					&SPA_RECTANGLE(INT32_MAX, INT32_MAX)),
				0);
		}
		*param = spa_pod_builder_pop(builder, &f[0]);
		break;
	case 2:
		if (PORT_IS_DSP(this, direction, port_id) ||
		    PORT_IS_CONTROL(this, direction, port_id))
			return 0;

		spa_pod_builder_push_object(builder, &f[0],
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_mjpg),
			0);
		if (width != 0 && height != 0) {
			spa_pod_builder_add(builder,
				SPA_FORMAT_VIDEO_size,     SPA_POD_CHOICE_RANGE_Rectangle(
					&SPA_RECTANGLE(width, height),
					&SPA_RECTANGLE(1, 1),
					&SPA_RECTANGLE(INT32_MAX, INT32_MAX)),
				0);
		}
		*param = spa_pod_builder_pop(builder, &f[0]);
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
	struct port *port, *other;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_log_debug(this->log, "%p: enum params port %d.%d %d %u",
			this, direction, port_id, seq, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(object, direction, port_id, result.index, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		if (PORT_IS_DSP(this, direction, port_id))
			param = spa_format_video_dsp_build(&b, id, &port->format.info.dsp);
		else if (PORT_IS_CONTROL(this, direction, port_id))
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Format,  id,
				SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_application),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control),
				SPA_FORMAT_CONTROL_types,  SPA_POD_Int(
					(1u<<SPA_CONTROL_UMP) | (1u<<SPA_CONTROL_Properties)));
		else
			param = spa_format_video_build(&b, id, &port->format);
		break;
	case SPA_PARAM_Buffers:
	{
		uint32_t size, min, max;

		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		if (PORT_IS_DSP(this, direction, port_id)) {
			size = 1024 * 1024 * 16;
		} else {
			size = 1024 * 1024 * 4;
		}

		other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);
		if (other->n_buffers > 0) {
			min = max = other->n_buffers;
		} else {
			min = 2;
			max = MAX_BUFFERS;
		}

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, min, max),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								size * port->stride,
								16 * port->stride,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(port->stride));
		break;
	}
	case SPA_PARAM_Meta:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Latency:
		switch (result.index) {
		case 0: case 1:
		{
			uint32_t idx = result.index;
			param = spa_latency_build(&b, id, &port->latency[idx]);
			break;
		}
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Tag:
		switch (result.index) {
		case 0: case 1:
		{
			uint32_t idx = result.index;
			if (port->is_monitor)
				idx = idx ^ 1;
			param = this->dir[idx].tag;
			if (param == NULL)
				goto next;
			break;
		}
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	if (param == NULL || spa_pod_filter(&b, &result.param, param, filter) < 0)
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

static int port_set_latency(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *latency)
{
	struct impl *this = object;
	struct port *port, *oport;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	struct spa_latency_info info;
	bool have_latency, emit = false;;
	uint32_t i;

	spa_log_debug(this->log, "%p: set latency direction:%d id:%d %p",
			this, direction, port_id, latency);

	port = GET_PORT(this, direction, port_id);
	if (latency == NULL) {
		info = SPA_LATENCY_INFO(other);
		have_latency = false;
	} else {
		if (spa_latency_parse(latency, &info) < 0 ||
		    info.direction != other)
			return -EINVAL;
		have_latency = true;
	}
	emit = spa_latency_info_compare(&info, &port->latency[other]) != 0 ||
	    port->have_latency == have_latency;

	port->latency[other] = info;
	port->have_latency = have_latency;

	spa_log_debug(this->log, "%p: set %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, this,
			info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
			info.min_quantum, info.max_quantum,
			info.min_rate, info.max_rate,
			info.min_ns, info.max_ns);

	if (this->monitor_passthrough) {
		if (port->is_monitor)
			oport = GET_PORT(this, other, port_id-1);
		else if (this->monitor && direction == SPA_DIRECTION_INPUT)
			oport = GET_PORT(this, other, port_id+1);
		else
			return 0;

		if (oport != NULL &&
		    spa_latency_info_compare(&info, &oport->latency[other]) != 0) {
			oport->latency[other] = info;
			oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			oport->params[IDX_Latency].user++;
			emit_port_info(this, oport, false);
		}
	} else {
		spa_latency_info_combine_start(&info, other);
		for (i = 0; i < this->dir[direction].n_ports; i++) {
			oport = GET_PORT(this, direction, i);
			if ((oport->is_monitor) || !oport->have_latency)
				continue;
			spa_log_debug(this->log, "%p: combine %d", this, i);
			spa_latency_info_combine(&info, &oport->latency[other]);
		}
		spa_latency_info_combine_finish(&info);

		spa_log_debug(this->log, "%p: combined %s latency %f-%f %d-%d %"PRIu64"-%"PRIu64, this,
				info.direction == SPA_DIRECTION_INPUT ? "input" : "output",
				info.min_quantum, info.max_quantum,
				info.min_rate, info.max_rate,
				info.min_ns, info.max_ns);

		for (i = 0; i < this->dir[other].n_ports; i++) {
			oport = GET_PORT(this, other, i);
			if (oport->is_monitor)
				continue;
			spa_log_debug(this->log, "%p: change %d", this, i);
			if (spa_latency_info_compare(&info, &oport->latency[other]) != 0) {
				oport->latency[other] = info;
				oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
				oport->params[IDX_Latency].user++;
				emit_port_info(this, oport, false);
			}
		}
	}
	if (emit) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].user++;
		emit_port_info(this, port, false);
	}
	return 0;
}

static int port_set_tag(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *tag)
{
	struct impl *this = object;
	struct port *port, *oport;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	uint32_t i;

	spa_log_debug(this->log, "%p: set tag direction:%d id:%d %p",
			this, direction, port_id, tag);

	port = GET_PORT(this, direction, port_id);
	if (port->is_monitor && !this->monitor_passthrough)
		return 0;

	if (tag != NULL) {
		struct spa_tag_info info;
		void *state = NULL;
		if (spa_tag_parse(tag, &info, &state) < 0 ||
		    info.direction != other)
			return -EINVAL;
	}
	if (spa_tag_compare(tag, this->dir[other].tag) != 0) {
		free(this->dir[other].tag);
		this->dir[other].tag = tag ? spa_pod_copy(tag) : NULL;

		for (i = 0; i < this->dir[other].n_ports; i++) {
			oport = GET_PORT(this, other, i);
			oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			oport->params[IDX_Tag].user++;
			emit_port_info(this, oport, false);
		}
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[IDX_Tag].user++;
	emit_port_info(this, port, false);
	return 0;
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

	spa_log_debug(this->log, "%p: %d:%d set format", this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_video_info info = { 0 };
		spa_debug_format(2, NULL, format);

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0) {
			spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
			return res;
		}
		if (PORT_IS_DSP(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_video ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_dsp) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_video_dsp_parse(format, &info.info.dsp)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			if (info.info.dsp.format != SPA_VIDEO_FORMAT_DSP_F32) {
				spa_log_error(this->log, "unexpected format %d<->%d",
					info.info.dsp.format, SPA_VIDEO_FORMAT_DSP_F32);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 16;
		}
		else if (PORT_IS_CONTROL(this, direction, port_id)) {
			if (info.media_type != SPA_MEDIA_TYPE_application ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_control) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			port->blocks = 1;
			port->stride = 1;
		}
		else {
			struct dir *dir = &this->dir[direction];
			struct dir *odir = &this->dir[SPA_DIRECTION_REVERSE(direction)];

			if (info.media_type != SPA_MEDIA_TYPE_video) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_video_parse(format, &info)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			port->stride = 2;
			port->stride *= info.info.raw.size.width;
			port->blocks = 1;
			dir->format = info;
			dir->have_format = true;
			if (odir->have_format) {
				if (memcmp(&odir->format, &dir->format, sizeof(dir->format)) == 0)
					this->fmt_passthrough = true;
			}
			this->setup = false;
		}
		port->format = info;
		port->have_format = true;

		spa_log_debug(this->log, "%p: %d %d %d", this,
				port_id, port->stride, port->blocks);
	}

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	SPA_FLAG_UPDATE(port->info.flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS, this->fmt_passthrough);

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->have_format) {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
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

	spa_log_debug(this->log, "%p: set param port %d.%d %u",
			this, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Latency:
		return port_set_latency(this, direction, port_id, flags, param);
	case SPA_PARAM_Tag:
		return port_set_tag(this, direction, port_id, flags, param);
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static inline void queue_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	spa_log_trace_fp(this->log, "%p: queue buffer %d on port %d %d",
			this, id, port->id, b->flags);
	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
}

static inline struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_log_trace_fp(this->log, "%p: peek buffer %d/%d on port %d %u",
			this, b->id, port->n_buffers, port->id, b->flags);
	return b;
}

static inline void dequeue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	spa_log_trace_fp(this->log, "%p: dequeue buffer %d on port %d %u",
			this, b->id, port->id, b->flags);
	if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_QUEUED))
		return;
	spa_list_remove(&b->link);
	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_QUEUED);
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
	uint32_t i, j, maxsize;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_log_debug(this->log, "%p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	clear_buffers(this, port);

	if (n_buffers > 0 && !port->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	maxsize = this->quantum_limit * sizeof(float);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		uint32_t n_datas = buffers[i]->n_datas;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->buf = buffers[i];

		if (n_datas != port->blocks) {
			spa_log_error(this->log, "%p: invalid blocks %d on buffer %d",
					this, n_datas, i);
			return -EINVAL;
		}
		if (SPA_FLAG_IS_SET(flags, SPA_NODE_BUFFERS_FLAG_ALLOC)) {
			struct port *other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

			if (other->n_buffers <= 0)
				return -EIO;
			*b->buf = *other->buffers[i % other->n_buffers].buf;
			b->datas[0] = other->buffers[i % other->n_buffers].datas[0];
		} else {
			for (j = 0; j < n_datas; j++) {
				void *data = d[j].data;
				if (data == NULL && SPA_FLAG_IS_SET(d[j].flags, SPA_DATA_FLAG_MAPPABLE)) {
					data = mmap(NULL, d[j].maxsize,
						PROT_READ, MAP_SHARED, d[j].fd,
						d[j].mapoffset);
					if (data == MAP_FAILED) {
						spa_log_error(this->log, "%p: mmap failed %d on buffer %d %d %p: %m",
								this, j, i, d[j].type, data);
						return -EINVAL;
					}
				}
				if (data != NULL && !SPA_IS_ALIGNED(data, this->max_align)) {
					spa_log_warn(this->log, "%p: memory %d on buffer %d not aligned",
							this, j, i);
				}
				b->datas[j] = data;
				maxsize = SPA_MAX(maxsize, d[j].maxsize);
			}
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, i);
	}
	port->maxsize = maxsize;
	port->n_buffers = n_buffers;

	return 0;
}

struct io_data {
	struct port *port;
	void *data;
	size_t size;
};

static int do_set_port_io(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct io_data *d = user_data;
	d->port->io = d->data;
	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set io %d on port %d:%d %p",
			this, id, direction, port_id, data);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		if (this->data_loop) {
			struct io_data d = { .port = port, .data = data, .size = size };
			spa_loop_invoke(this->data_loop, do_set_port_io, 0, NULL, 0, true, &d);
		}
		else
			port->io = data;
		break;
	case SPA_IO_RateMatch:
		this->io_rate_match = data;
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

	port = GET_OUT_PORT(this, port_id);
	queue_buffer(this, port, buffer_id);

	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *in_port, *out_port;
	struct spa_io_buffers *input, *output;
	struct buffer *dbuf, *sbuf;
	struct dir *in, *out;
	struct AVFrame *f;
	void *datas[8];
	uint32_t sizes[8], strides[8];
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	in = &this->dir[SPA_DIRECTION_INPUT];
	out = &this->dir[SPA_DIRECTION_OUTPUT];

	out_port = GET_OUT_PORT(this, 0);
	if ((output = out_port->io) == NULL)
		return -EIO;

	if (output->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	/* recycle */
	if (output->buffer_id < out_port->n_buffers) {
		queue_buffer(this, out_port, output->buffer_id);
		output->buffer_id = SPA_ID_INVALID;
	}

	in_port = GET_IN_PORT(this, 0);
	if ((input = in_port->io) == NULL)
		return -EIO;

	if (input->status != SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_NEED_DATA;

	if (input->buffer_id >= in_port->n_buffers) {
		input->status = -EINVAL;
		return -EINVAL;
	}

	sbuf = &in_port->buffers[input->buffer_id];
	input->status = SPA_STATUS_NEED_DATA;

	if ((dbuf = peek_buffer(this, out_port)) == NULL) {
                spa_log_error(this->log, "%p: out of buffers", this);
		return -EPIPE;
	}
	dbuf = &out_port->buffers[input->buffer_id];

	spa_log_trace(this->log, "%d %p:%p %d %d %d", input->buffer_id, sbuf->buf->datas[0].chunk,
			dbuf->buf->datas[0].chunk, sbuf->buf->datas[0].chunk->size,
			sbuf->id, dbuf->id);

	/* do decoding */
	if (this->decoder.codec) {
		this->decoder.packet->data = sbuf->datas[0];
		this->decoder.packet->size = sbuf->buf->datas[0].chunk->size;

		if ((res = avcodec_send_packet(this->decoder.context, this->decoder.packet)) < 0) {
			spa_log_error(this->log, "failed to send frame to codec: %d %p:%d",
					res, this->decoder.packet->data, this->decoder.packet->size);
			return -EIO;
		}

		f = this->decoder.frame;
		if (avcodec_receive_frame(this->decoder.context, f) < 0) {
			spa_log_error(this->log, "failed to receive frame from codec");
			return -EIO;
		}

		in->pix_fmt = f->format;
		in->width = f->width;
		in->height = f->height;
	} else {
		f = this->decoder.frame;
		f->format = in->pix_fmt;
		f->width = in->width;
		f->height = in->height;
		f->data[0] = sbuf->datas[0];
		f->linesize[0] = sbuf->buf->datas[0].chunk->stride;
	}

	/* do conversion */
	if (f->format != out->pix_fmt ||
	    f->width != out->width ||
	    f->height != out->height) {
		if (this->convert.context == NULL) {
			this->convert.context = sws_getContext(
					f->width, f->height, f->format,
					out->width, out->height, out->pix_fmt,
					0, NULL, NULL, NULL);
		}
		sws_scale_frame(this->convert.context, this->convert.frame, f);
		f = this->convert.frame;
	}
	/* do encoding */
	if (this->encoder.codec) {
		if ((res = avcodec_send_frame(this->encoder.context, f)) < 0) {
			spa_log_error(this->log, "failed to send frame to codec: %d", res);
			return -EIO;
		}
		if (avcodec_receive_packet(this->encoder.context, this->encoder.packet) < 0) {
			spa_log_error(this->log, "failed to receive frame from codec");
			return -EIO;
		}
		datas[0] = this->encoder.packet->data;
		sizes[0] = this->encoder.packet->size;
		strides[0] = 1;

	} else {
		datas[0] = f->data[0];
		strides[0] = f->linesize[0];
		sizes[0] = strides[0] * out->height;
	}

	/* write to output */
	for (uint_fast32_t i = 0; i < dbuf->buf->n_datas; ++i) {
		if (SPA_FLAG_IS_SET(dbuf->buf->datas[i].flags, SPA_DATA_FLAG_DYNAMIC))
			dbuf->buf->datas[i].data = datas[i];
		else if (datas[i] && dbuf->datas[i] && dbuf->datas[i] != datas[i])
			memcpy(dbuf->datas[i], datas[i], sizes[i]);

		if (dbuf->buf->datas[i].chunk != sbuf->buf->datas[i].chunk) {
			dbuf->buf->datas[i].chunk->stride = strides[i];
			dbuf->buf->datas[i].chunk->size = sizes[i];
		}
	}

	dequeue_buffer(this, out_port, dbuf);
	output->buffer_id = dbuf->id;
	output->status = SPA_STATUS_HAVE_DATA;

	return SPA_STATUS_HAVE_DATA;
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

static void free_dir(struct dir *dir)
{
	uint32_t i;
	for (i = 0; i < MAX_PORTS; i++)
		free(dir->ports[i]);
	free(dir->tag);
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	free_dir(&this->dir[SPA_DIRECTION_INPUT]);
	free_dir(&this->dir[SPA_DIRECTION_OUTPUT]);
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
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);

	this->cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (this->cpu) {
		this->cpu_flags = spa_cpu_get_flags(this->cpu);
		this->max_align = SPA_MIN(MAX_ALIGN, spa_cpu_get_max_align(this->cpu));
	}
	props_reset(&this->props);

	this->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	this->rate_limit.burst = 1;

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit"))
			spa_atou32(s, &this->quantum_limit, 0);
		else if (spa_streq(k, SPA_KEY_PORT_IGNORE_LATENCY))
			this->port_ignore_latency = spa_atob(s);
		else if (spa_streq(k, SPA_KEY_PORT_GROUP))
			spa_scnprintf(this->group_name, sizeof(this->group_name), "%s", s);
		else if (spa_streq(k, "monitor.passthrough"))
			this->monitor_passthrough = spa_atob(s);
		else
			videoconvert_set_param(this, k, s);
	}

	this->dir[SPA_DIRECTION_INPUT].direction = SPA_DIRECTION_INPUT;
	this->dir[SPA_DIRECTION_OUTPUT].direction = SPA_DIRECTION_OUTPUT;

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = MAX_PORTS;
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_IN_PORT_CONFIG |
		SPA_NODE_FLAG_OUT_PORT_CONFIG |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[IDX_PortConfig] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_INPUT, false, false, NULL);
	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_convert, SPA_DIRECTION_OUTPUT, false, false, NULL);

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

const struct spa_handle_factory spa_videoconvert_ffmpeg_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_VIDEO_CONVERT".ffmpeg",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
