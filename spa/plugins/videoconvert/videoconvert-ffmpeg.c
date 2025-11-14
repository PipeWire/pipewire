/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/mman.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>

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
#include <spa/param/peer-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/debug/types.h>
#include <spa/debug/format.h>
#include <spa/debug/pod.h>
#include <spa/debug/log.h>
#include <spa/control/ump-utils.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.videoconvert.ffmpeg");

#define MAX_ALIGN	64u
#define MAX_BUFFERS	32u
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
#define BUFFER_FLAG_MAPPED	(1<<1)
	uint32_t flags;
	struct spa_list link;
	struct spa_buffer *buf;
	void *datas[MAX_DATAS];
	struct spa_meta_header *h;
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;

#define IDX_EnumFormat		0
#define IDX_Meta		1
#define IDX_IO			2
#define IDX_Format		3
#define IDX_Buffers		4
#define IDX_Latency		5
#define IDX_Tag			6
#define IDX_PeerEnumFormat	7
#define IDX_PeerCapability	8
#define N_PORT_PARAMS		9
	struct spa_param_info params[N_PORT_PARAMS];

	struct spa_pod *peer_format_pod;
	const struct spa_pod **peer_formats;
	uint32_t n_peer_formats;

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
	uint32_t size;
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
	struct spa_rectangle size;
	struct spa_fraction framerate;

	ptrdiff_t linesizes[4];
	size_t plane_size[4];

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
		AVCodecContext *context;
		AVPacket *packet;
		AVFrame *frame;
	} decoder;
	struct {
		struct SwsContext *context;
		AVFrame *frame;
	} convert;
	struct {
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
	}
	this->info.change_mask = old;
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
	}
	port->info.change_mask = old;
}

static void emit_info(struct impl *this, bool full)
{
	struct port *p;
	uint32_t i;

	emit_node_info(this, full);
	for (i = 0; i < this->dir[SPA_DIRECTION_INPUT].n_ports; i++) {
		if ((p = GET_IN_PORT(this, i)) && p->valid)
			emit_port_info(this, p, full);
	}
	for (i = 0; i < this->dir[SPA_DIRECTION_OUTPUT].n_ports; i++) {
		if ((p = GET_OUT_PORT(this, i)) && p->valid)
			emit_port_info(this, p, full);
	}
}

struct format_info {
	enum AVPixelFormat pix_fmt;
	uint32_t format;
	uint32_t dsp_format;
#define FORMAT_DSP	(1<<0)
#define FORMAT_COMMON	(1<<1)
	uint32_t flags;
};

#if defined AV_PIX_FMT_AYUV
#define VIDEO_FORMAT_DSP_AYUV SPA_VIDEO_FORMAT_AYUV
#else
#define VIDEO_FORMAT_DSP_AYUV SPA_VIDEO_FORMAT_Y444
#endif
#define VIDEO_FORMAT_DSP_RGBA SPA_VIDEO_FORMAT_RGBA

static struct format_info format_info[] =
{
#if defined AV_PIX_FMT_AYUV
	{ AV_PIX_FMT_AYUV,  SPA_VIDEO_FORMAT_AYUV, VIDEO_FORMAT_DSP_AYUV, FORMAT_DSP | FORMAT_COMMON },
#else
	{ AV_PIX_FMT_YUV444P, SPA_VIDEO_FORMAT_Y444, VIDEO_FORMAT_DSP_AYUV, FORMAT_DSP | FORMAT_COMMON },
#endif
	{ AV_PIX_FMT_RGBA,  SPA_VIDEO_FORMAT_RGBA, VIDEO_FORMAT_DSP_RGBA, FORMAT_DSP | FORMAT_COMMON },

	{ AV_PIX_FMT_YUYV422, SPA_VIDEO_FORMAT_YUY2, VIDEO_FORMAT_DSP_AYUV, FORMAT_COMMON },
	{ AV_PIX_FMT_UYVY422, SPA_VIDEO_FORMAT_UYVY, VIDEO_FORMAT_DSP_AYUV, FORMAT_COMMON },
	{ AV_PIX_FMT_YVYU422, SPA_VIDEO_FORMAT_YVYU, VIDEO_FORMAT_DSP_AYUV, FORMAT_COMMON },
	{ AV_PIX_FMT_YUV420P, SPA_VIDEO_FORMAT_I420, VIDEO_FORMAT_DSP_AYUV, FORMAT_COMMON },

	{ AV_PIX_FMT_BGR0,  SPA_VIDEO_FORMAT_BGRx, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },
	{ AV_PIX_FMT_BGRA,  SPA_VIDEO_FORMAT_BGRA, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },
	{ AV_PIX_FMT_ARGB,  SPA_VIDEO_FORMAT_ARGB, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },
	{ AV_PIX_FMT_ABGR,  SPA_VIDEO_FORMAT_ABGR, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },

	//{ AV_PIX_FMT_NONE,  SPA_VIDEO_FORMAT_YV12 },

	{ AV_PIX_FMT_RGB0,  SPA_VIDEO_FORMAT_RGBx, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },
	{ AV_PIX_FMT_0RGB,  SPA_VIDEO_FORMAT_xRGB, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },
	{ AV_PIX_FMT_0BGR,  SPA_VIDEO_FORMAT_xBGR, VIDEO_FORMAT_DSP_RGBA, FORMAT_COMMON },

	//{ AV_PIX_FMT_RGB24,   SPA_VIDEO_FORMAT_RGB },
	//{ AV_PIX_FMT_BGR24,   SPA_VIDEO_FORMAT_BGR },
	//{ AV_PIX_FMT_YUV411P, SPA_VIDEO_FORMAT_Y41B },
	//{ AV_PIX_FMT_YUV422P, SPA_VIDEO_FORMAT_Y42B },

	//{ AV_PIX_FMT_NONE,  SPA_VIDEO_FORMAT_v210 },
	//{ AV_PIX_FMT_NONE,  SPA_VIDEO_FORMAT_v216 },

	//{ AV_PIX_FMT_NV12,    SPA_VIDEO_FORMAT_NV12 },
	//{ AV_PIX_FMT_NV21,    SPA_VIDEO_FORMAT_NV21 },
	//{ AV_PIX_FMT_GRAY8,    SPA_VIDEO_FORMAT_GRAY8 },
	//{ AV_PIX_FMT_GRAY16BE, SPA_VIDEO_FORMAT_GRAY16_BE },
	//{ AV_PIX_FMT_GRAY16LE, SPA_VIDEO_FORMAT_GRAY16_LE },
	//{ AV_PIX_FMT_NONE,     SPA_VIDEO_FORMAT_v308 },
	//{ AV_PIX_FMT_RGB565,   SPA_VIDEO_FORMAT_RGB16 },
	//{ AV_PIX_FMT_NONE,     SPA_VIDEO_FORMAT_BGR16 },
	//{ AV_PIX_FMT_RGB555,   SPA_VIDEO_FORMAT_RGB15 },
	//{ AV_PIX_FMT_NONE,     SPA_VIDEO_FORMAT_BGR15 },
	//{ AV_PIX_FMT_NONE,     SPA_VIDEO_FORMAT_UYVP },
	//{ AV_PIX_FMT_YUVA420P, SPA_VIDEO_FORMAT_A420 },
	//{ AV_PIX_FMT_PAL8,     SPA_VIDEO_FORMAT_RGB8P },
	//{ AV_PIX_FMT_YUV410P,  SPA_VIDEO_FORMAT_YUV9 },

	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_YVU9 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_IYU1 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_ARGB64 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_AYUV64 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_r210 },

	//{ AV_PIX_FMT_YUV420P10BE, SPA_VIDEO_FORMAT_I420_10BE },
	//{ AV_PIX_FMT_YUV420P10LE, SPA_VIDEO_FORMAT_I420_10LE },
	//{ AV_PIX_FMT_YUV422P10BE, SPA_VIDEO_FORMAT_I422_10BE },
	//{ AV_PIX_FMT_YUV422P10LE, SPA_VIDEO_FORMAT_I422_10LE },
	//{ AV_PIX_FMT_YUV444P10BE, SPA_VIDEO_FORMAT_Y444_10BE },
	//{ AV_PIX_FMT_YUV444P10LE, SPA_VIDEO_FORMAT_Y444_10LE },
	//{ AV_PIX_FMT_GBRP,        SPA_VIDEO_FORMAT_GBR },
	//{ AV_PIX_FMT_GBRP10BE,    SPA_VIDEO_FORMAT_GBR_10BE },
	//{ AV_PIX_FMT_GBRP10LE,    SPA_VIDEO_FORMAT_GBR_10LE },

	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_NV16 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_NV24 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_NV12_64Z32 },

	//{ AV_PIX_FMT_YUVA420P10BE, SPA_VIDEO_FORMAT_A420_10BE },
	//{ AV_PIX_FMT_YUVA420P10LE, SPA_VIDEO_FORMAT_A420_10LE },
	//{ AV_PIX_FMT_YUVA422P10BE, SPA_VIDEO_FORMAT_A422_10BE },
	//{ AV_PIX_FMT_YUVA422P10LE, SPA_VIDEO_FORMAT_A422_10LE },
	//{ AV_PIX_FMT_YUVA444P10BE, SPA_VIDEO_FORMAT_A444_10BE },
	//{ AV_PIX_FMT_YUVA444P10LE, SPA_VIDEO_FORMAT_A444_10LE },

	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_NV61 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_P010_10BE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_P010_10LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_IYU2 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_VYUY },

	//{ AV_PIX_FMT_GBRAP, SPA_VIDEO_FORMAT_GBRA },
	//{ AV_PIX_FMT_GBRAP10BE, SPA_VIDEO_FORMAT_GBRA_10BE },
	//{ AV_PIX_FMT_GBRAP10LE, SPA_VIDEO_FORMAT_GBRA_10LE },
	//{ AV_PIX_FMT_GBRP12BE, SPA_VIDEO_FORMAT_GBR_12BE },
	//{ AV_PIX_FMT_GBRP12LE, SPA_VIDEO_FORMAT_GBR_12LE },
	//{ AV_PIX_FMT_GBRAP12BE, SPA_VIDEO_FORMAT_GBRA_12BE },
	//{ AV_PIX_FMT_GBRAP12LE, SPA_VIDEO_FORMAT_GBRA_12LE },
	//{ AV_PIX_FMT_YUV420P12BE, SPA_VIDEO_FORMAT_I420_12BE },
	//{ AV_PIX_FMT_YUV420P12LE, SPA_VIDEO_FORMAT_I420_12LE },
	//{ AV_PIX_FMT_YUV422P12BE, SPA_VIDEO_FORMAT_I422_12BE },
	//{ AV_PIX_FMT_YUV422P12LE, SPA_VIDEO_FORMAT_I422_12LE },
	//{ AV_PIX_FMT_YUV444P12BE, SPA_VIDEO_FORMAT_Y444_12BE },
	//{ AV_PIX_FMT_YUV444P12LE, SPA_VIDEO_FORMAT_Y444_12LE },

	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_RGBA_F16 },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_RGBA_F32 },

	//{ AV_PIX_FMT_X2RGB10LE, SPA_VIDEO_FORMAT_xRGB_210LE },
	//{ AV_PIX_FMT_X2BGR10LE, SPA_VIDEO_FORMAT_xBGR_210LE },

	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_RGBx_102LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_BGRx_102LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_ARGB_210LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_ABGR_210LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_RGBA_102LE },
	//{ AV_PIX_FMT_NONE, SPA_VIDEO_FORMAT_BGRA_102LE },
};

static struct format_info *format_info_for_format(uint32_t format)
{
	SPA_FOR_EACH_ELEMENT_VAR(format_info, i) {
		if (i->format == format)
			return i;
	}
	return NULL;
}

static enum AVPixelFormat format_to_pix_fmt(uint32_t format)
{
	struct format_info *i = format_info_for_format(format);
	return i ? i->pix_fmt : AV_PIX_FMT_NONE;
}

static int get_format(struct dir *dir, uint32_t *format, struct spa_rectangle *size,
		struct spa_fraction *framerate)
{
	if (dir->have_format) {
		switch (dir->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_dsp:
			*format = dir->format.info.dsp.format;
			*size = SPA_RECTANGLE(640, 480);
			*framerate = SPA_FRACTION(30, 1);
			break;
		case SPA_MEDIA_SUBTYPE_raw:
			*format = dir->format.info.raw.format;
			*size = dir->format.info.raw.size;
			*framerate = dir->format.info.raw.framerate;
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			*format = SPA_VIDEO_FORMAT_I420;
			*size = dir->format.info.mjpg.size;
			*framerate = dir->format.info.mjpg.framerate;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			*format = SPA_VIDEO_FORMAT_I420;
			*size = dir->format.info.h264.size;
			*framerate = dir->format.info.h264.framerate;
			break;
		default:
			*format = SPA_VIDEO_FORMAT_I420;
			*size = SPA_RECTANGLE(640, 480);
			*framerate = SPA_FRACTION(30, 1);
			break;
		}
	} else {
		*format = SPA_VIDEO_FORMAT_I420;
		*size = SPA_RECTANGLE(640, 480);
		*framerate = SPA_FRACTION(30, 1);
	}
	return 0;
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

	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask = port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info.flags = SPA_PORT_FLAG_NO_REF |
		SPA_PORT_FLAG_DYNAMIC_DATA;
	port->params[IDX_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[IDX_Meta] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	port->params[IDX_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[IDX_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[IDX_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->params[IDX_Latency] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READWRITE);
	port->params[IDX_Tag] = SPA_PARAM_INFO(SPA_PARAM_Tag, SPA_PARAM_INFO_READWRITE);
	port->params[IDX_PeerEnumFormat] = SPA_PARAM_INFO(SPA_PARAM_PeerEnumFormat, SPA_PARAM_INFO_WRITE);
	port->params[IDX_PeerCapability] = SPA_PARAM_INFO(SPA_PARAM_PeerCapability, SPA_PARAM_INFO_WRITE);
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

	return 0;
}

static int deinit_port(struct impl *this, enum spa_direction direction, uint32_t port_id)
{
	struct port *port = GET_PORT(this, direction, port_id);
	if (port == NULL || !port->valid)
		return -ENOENT;
	port->valid = false;
	free(port->peer_formats);
	port->peer_formats = NULL;
	port->n_peer_formats = 0;
	free(port->peer_format_pod);
	port->peer_format_pod = NULL;
	spa_node_emit_port_info(&this->hooks, direction, port_id, NULL);
	return 0;
}

static int node_param_enum_port_config(struct impl *this, uint32_t id, uint32_t index,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0 ... 1:
	{
		struct dir *dir = &this->dir[index];
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamPortConfig, id,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_CHOICE_ENUM_Id(4,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_none,
				SPA_PARAM_PORT_CONFIG_MODE_dsp,
				SPA_PARAM_PORT_CONFIG_MODE_convert),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_CHOICE_Bool(false),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_CHOICE_Bool(false));
		return 1;
	}
	}
	return 0;
}

static int node_param_port_config(struct impl *this, uint32_t id, uint32_t index,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0 ... 1:
	{
		struct dir *dir = &this->dir[index];;
		struct spa_pod_frame f[1];
		spa_pod_builder_push_object(b, &f[0],
				SPA_TYPE_OBJECT_ParamPortConfig, id);
		spa_pod_builder_add(b,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(dir->mode),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(this->monitor),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(dir->control),
			0);

		if (dir->have_format) {
			spa_pod_builder_prop(b, SPA_PARAM_PORT_CONFIG_format, 0);
			spa_format_video_build(b, id, &dir->format);
		}
		*param = spa_pod_builder_pop(b, &f[0]);
		return 1;
	}
	}
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
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	param = NULL;
	switch (id) {
	case SPA_PARAM_EnumPortConfig:
		res = node_param_enum_port_config(this, id, result.index, &param, &b);
		break;
	case SPA_PARAM_PortConfig:
		res = node_param_port_config(this, id, result.index, &param, &b);
		break;
	case SPA_PARAM_PropInfo:
		res = 0;
		break;
	case SPA_PARAM_Props:
		res = 0;
		break;
	default:
		return 0;
	}
	if (res <= 0)
		return res;

	if (param == NULL || spa_pod_filter(&b, &result.param, param, filter) < 0)
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
		dir->n_ports = 1;
		dir->format.info.dsp = SPA_VIDEO_INFO_DSP_INIT(
				.format = SPA_VIDEO_FORMAT_DSP_F32);
		dir->have_format = true;

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

	/* emit all port info first, then the node props and flags */
	emit_info(this, false);

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_PortConfig].user++;

	return 0;
}

static int node_set_param_port_config(struct impl *this, uint32_t flags,
				const struct spa_pod *param)
{
	struct spa_video_info info = { 0, }, *infop = NULL;
	struct spa_pod *format = NULL;
	enum spa_direction direction;
	enum spa_param_port_config_mode mode;
	bool monitor = false, control = false;
	int res;

	if (param == NULL)
		return 0;

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
	return reconfigure_mode(this, mode, direction, monitor, control, infop);
}

static int node_set_param_props(struct impl *this, uint32_t flags,
				const struct spa_pod *param)
{
	if (param == NULL)
		return 0;

	apply_props(this, param);
	return 0;
}
static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_PortConfig:
		res = node_set_param_port_config(this, flags, param);
		break;
	case SPA_PARAM_Props:
		res = node_set_param_props(this, flags, param);
		break;
	default:
		return -ENOENT;
	}
	emit_info(this, false);
	return res;
}

static inline void free_decoder(struct impl *this)
{
	avcodec_free_context(&this->decoder.context);
	av_packet_free(&this->decoder.packet);
}

static inline void free_encoder(struct impl *this)
{
	avcodec_free_context(&this->encoder.context);
	av_packet_free(&this->encoder.packet);
	av_frame_free(&this->encoder.frame);
}

static int setup_convert(struct impl *this)
{
	struct dir *in, *out;
	uint32_t in_format = 0, out_format = 0;
	int decoder_id = 0, encoder_id = 0;
	const AVCodec *codec;

	in = &this->dir[SPA_DIRECTION_INPUT];
	out = &this->dir[SPA_DIRECTION_OUTPUT];

	spa_log_debug(this->log, "%p: setup:%d in_format:%d out_format:%d", this,
			this->setup, in->have_format, out->have_format);

	if (this->setup)
		return 0;

	if (!in->have_format || !out->have_format)
		return -EIO;

	get_format(in, &in_format, &in->size, &in->framerate);
	get_format(out, &out_format, &out->size, &out->framerate);

	switch (in->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_dsp:
	case SPA_MEDIA_SUBTYPE_raw:
		in->pix_fmt = format_to_pix_fmt(in_format);
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
		case SPA_MEDIA_SUBTYPE_dsp:
			out->pix_fmt = format_to_pix_fmt(out_format);
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			encoder_id = AV_CODEC_ID_MJPEG;
			out->format.media_subtype = SPA_MEDIA_SUBTYPE_raw;
			out->format.info.raw.format = SPA_VIDEO_FORMAT_I420;
			out->format.info.raw.size = in->size;
			out->format.info.raw.framerate = in->framerate;
			out->pix_fmt = AV_PIX_FMT_YUVJ420P;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			encoder_id = AV_CODEC_ID_H264;
			out->format.media_subtype = SPA_MEDIA_SUBTYPE_raw;
			out->format.info.raw.format = SPA_VIDEO_FORMAT_I420;
			out->format.info.raw.size = in->size;
			out->format.info.raw.framerate = in->framerate;
			out->pix_fmt = AV_PIX_FMT_YUVJ420P;
			break;
		default:
			spa_log_warn(this->log, "%p: in_subtype:%d out_subtype:%d", this,
					in->format.media_subtype, out->format.media_subtype);
			return -ENOTSUP;
		}
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_mjpg:
			/* passthrough if same dimensions or else reencode */
			if (in->size.width != out->size.width ||
			    in->size.height != out->size.height) {
				encoder_id = decoder_id = AV_CODEC_ID_MJPEG;
				out->pix_fmt = AV_PIX_FMT_YUVJ420P;
			}
			break;
		case SPA_MEDIA_SUBTYPE_raw:
		case SPA_MEDIA_SUBTYPE_dsp:
			out->pix_fmt = format_to_pix_fmt(out_format);
			decoder_id = AV_CODEC_ID_MJPEG;
			break;
		default:
			spa_log_warn(this->log, "%p: in_subtype:%d out_subtype:%d", this,
					in->format.media_subtype, out->format.media_subtype);
			return -ENOTSUP;
		}
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		switch (out->format.media_subtype) {
		case SPA_MEDIA_SUBTYPE_h264:
			/* passthrough if same dimensions or else reencode */
			if (in->size.width != out->size.width ||
			    in->size.height != out->size.height)
				encoder_id = decoder_id = AV_CODEC_ID_H264;
			break;
		case SPA_MEDIA_SUBTYPE_raw:
		case SPA_MEDIA_SUBTYPE_dsp:
			out->pix_fmt = format_to_pix_fmt(out_format);
			decoder_id = AV_CODEC_ID_H264;
			break;
		default:
			spa_log_warn(this->log, "%p: in_subtype:%d out_subtype:%d", this,
					in->format.media_subtype, out->format.media_subtype);
			return -ENOTSUP;
		}
		break;
	default:
		spa_log_warn(this->log, "%p: in_subtype:%d out_subtype:%d", this,
				in->format.media_subtype, out->format.media_subtype);
		return -ENOTSUP;
	}

	if (in->pix_fmt == AV_PIX_FMT_NONE || out->pix_fmt == AV_PIX_FMT_NONE) {
		spa_log_warn(this->log, "%p: unsupported pixel format", this);
		return -ENOTSUP;
	}
	if (decoder_id) {
		if ((codec = avcodec_find_decoder(decoder_id)) == NULL) {
			spa_log_error(this->log, "failed to find %d decoder", decoder_id);
			return -ENOTSUP;
		}
		if ((this->decoder.context = avcodec_alloc_context3(codec)) == NULL)
			return -EIO;

		if ((this->decoder.packet = av_packet_alloc()) == NULL)
			return -EIO;

		this->decoder.context->flags2 |= AV_CODEC_FLAG2_FAST;

		if (avcodec_open2(this->decoder.context, codec, NULL) < 0) {
			spa_log_error(this->log, "failed to open decoder codec");
			return -EIO;
		}
		spa_log_info(this->log, "%p: using decoder %s", this, codec->name);
	} else {
		free_decoder(this);
	}
	av_frame_free(&this->decoder.frame);
	if ((this->decoder.frame = av_frame_alloc()) == NULL)
		return -EIO;
	if (encoder_id) {
		if ((codec = avcodec_find_encoder(encoder_id)) == NULL) {
			spa_log_error(this->log, "failed to find %d encoder", encoder_id);
			return -ENOTSUP;
		}
		if ((this->encoder.context = avcodec_alloc_context3(codec)) == NULL)
			return -EIO;

		if ((this->encoder.packet = av_packet_alloc()) == NULL)
			return -EIO;
		if ((this->encoder.frame = av_frame_alloc()) == NULL)
			return -EIO;

		this->encoder.context->flags2 |= AV_CODEC_FLAG2_FAST;
		this->encoder.context->time_base.num = 1;
		this->encoder.context->width = out->size.width;
		this->encoder.context->height = out->size.height;
		this->encoder.context->pix_fmt = out->pix_fmt;

		if (avcodec_open2(this->encoder.context, codec, NULL) < 0) {
			spa_log_error(this->log, "failed to open encoder codec");
			return -EIO;
		}
		spa_log_info(this->log, "%p: using encoder %s", this, codec->name);
	} else {
		free_encoder(this);
	}
	sws_freeContext(this->convert.context);
	this->convert.context = NULL;
	av_frame_free(&this->convert.frame);
	if ((this->convert.frame = av_frame_alloc()) == NULL)
		return -EIO;

	this->setup = true;

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
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_info(this, true);

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

static void add_video_formats(struct spa_pod_builder *b,
		uint32_t *vals, uint32_t n_vals, bool is_dsp)
{
	struct spa_pod_frame f[1];
	uint32_t ids[SPA_N_ELEMENTS(format_info) + 1], n_ids = 0, i, j, fmt;

	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
	spa_pod_builder_push_choice(b, &f[0], SPA_CHOICE_Enum, 0);

	if (n_vals == 0) {
		 vals = &format_info[0].format;
		 n_vals = 1;
	}
	/* all supported formats */
	for (i = 0; i < n_vals; i++) {
		struct format_info *fi = format_info_for_format(vals[i]);
		if (fi == NULL)
			continue;

		fmt = is_dsp ? fi->dsp_format : fi->format;

		for (j = 0; j < n_ids; j++) {
			if (ids[j] == fmt)
				break;
		}
		if (j == n_ids)
			ids[n_ids++] = fmt;
	}
	/* then add all other supported formats */
	SPA_FOR_EACH_ELEMENT_VAR(format_info, fi) {
		if (fi->pix_fmt == AV_PIX_FMT_NONE)
			continue;

		fmt = is_dsp ? fi->dsp_format : fi->format;

		for (j = 0; j < n_ids; j++) {
			if (fmt == ids[j])
				break;
		}
		if (j == n_ids)
			ids[n_ids++] = fmt;
	}
	for (i = 0; i < n_ids; i++) {
		spa_pod_builder_id(b, ids[i]);
		if (i == 0)
			spa_pod_builder_id(b, ids[i]);
	}
	spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *transform_format(struct impl *this, struct port *port, const struct spa_pod *format,
		uint32_t id, struct spa_pod_builder *b)
{
	uint32_t media_type, media_subtype, fmt;
	struct spa_pod_object *obj;
	const struct spa_pod_prop *prop;
	struct spa_pod_frame f[2];

	if (!spa_format_parse(format, &media_type, &media_subtype) ||
	    media_type != SPA_MEDIA_TYPE_video) {
		return NULL;
	}

	obj = (struct spa_pod_object*)format;
	spa_pod_builder_push_object(b, &f[0], obj->body.type, id);
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_FORMAT_mediaType:
			spa_pod_builder_prop(b, prop->key, prop->flags);
			spa_pod_builder_id(b, SPA_MEDIA_TYPE_video);
			break;
		case SPA_FORMAT_mediaSubtype:
			spa_pod_builder_prop(b, prop->key, prop->flags);
			spa_pod_builder_id(b, SPA_MEDIA_SUBTYPE_raw);
			fmt = SPA_VIDEO_FORMAT_I420;
			switch (media_subtype) {
			case SPA_MEDIA_SUBTYPE_raw:
				break;
			case SPA_MEDIA_SUBTYPE_mjpg:
				add_video_formats(b, &fmt, 1, port->is_dsp);
				break;
			case SPA_MEDIA_SUBTYPE_h264:
				add_video_formats(b, &fmt, 1, port->is_dsp);
				break;
			default:
				return NULL;
			}
			break;
		case SPA_FORMAT_VIDEO_format:
		{
			uint32_t n_vals, choice, *id_vals;
			struct spa_pod *val = spa_pod_get_values(&prop->value, &n_vals, &choice);

			if (n_vals < 1)
				return 0;

			if (!spa_pod_is_id(val))
				return 0;

			id_vals = SPA_POD_BODY(val);

			add_video_formats(b, id_vals, n_vals, port->is_dsp);
			break;
		}
		default:
			spa_pod_builder_raw_padded(b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static int diff_value(struct impl *impl, uint32_t type, uint32_t size, const void *v1, const void *v2)
{
	switch (type) {
	case SPA_TYPE_None:
		return INT_MAX;
	case SPA_TYPE_Bool:
		if (size < sizeof(int32_t))
			return INT_MAX;
		return (!!*(int32_t *)v1) - (!!*(int32_t *)v2);
	case SPA_TYPE_Id:
		if (size < sizeof(uint32_t))
			return INT_MAX;
		return (*(uint32_t *)v1) != (*(uint32_t *)v2);
	case SPA_TYPE_Int:
		if (size < sizeof(int32_t))
			return INT_MAX;
		return *(int32_t *)v1 - *(int32_t *)v2;
	case SPA_TYPE_Long:
		if (size < sizeof(int64_t))
			return INT_MAX;
		return *(int64_t *)v1 - *(int64_t *)v2;
	case SPA_TYPE_Float:
		if (size < sizeof(float))
			return INT_MAX;
		return (int)(*(float *)v1 - *(float *)v2);
	case SPA_TYPE_Double:
		if (size < sizeof(double))
			return INT_MAX;
		return (int)(*(double *)v1 - *(double *)v2);
	case SPA_TYPE_String:
		if (size < 1 ||
		    ((const char *)v1)[size - 1] != 0 ||
		    ((const char *)v2)[size - 1] != 0)
			return INT_MAX;
		return strcmp((char *)v1, (char *)v2);
	case SPA_TYPE_Bytes:
		return memcmp((char *)v1, (char *)v2, size);
	case SPA_TYPE_Rectangle:
	{
		const struct spa_rectangle *rec1, *rec2;
		uint64_t n1, n2;

		if (size < sizeof(*rec1))
			return INT_MAX;
		rec1 = (struct spa_rectangle *) v1;
		rec2 = (struct spa_rectangle *) v2;
		n1 = ((uint64_t) rec1->width) * rec1->height;
		n2 = ((uint64_t) rec2->width) * rec2->height;
		if (rec1->width == rec2->width && rec1->height == rec2->height)
			return 0;
		else if (n1 < n2)
			return -(n2 - n1);
		else if (n1 > n2)
			return n1 - n2;
		else if (rec1->width == rec2->width)
			return (int32_t)rec1->height - (int32_t)rec2->height;
		else
			return (int32_t)rec1->width - (int32_t)rec2->width;
	}
	case SPA_TYPE_Fraction:
	{
		const struct spa_fraction *f1, *f2;
		uint64_t n1, n2;

		if (size < sizeof(*f1))
			return INT_MAX;
		f1 = (struct spa_fraction *) v1;
		f2 = (struct spa_fraction *) v2;
		n1 = ((uint64_t) f1->num) * f2->denom;
		n2 = ((uint64_t) f2->num) * f1->denom;
		return (int) (n1 - n2);
	}
	default:
		break;
	}
	return 0;
}

static int diff_prop(struct impl *impl, struct spa_pod_prop *prop,
		uint32_t type, const void *target, bool fix)
{
	uint32_t i, n_vals, choice, size;
	struct spa_pod *val = spa_pod_get_values(&prop->value, &n_vals, &choice);
	void *vals, *v, *best = NULL;
	int res = INT_MAX;

	if (n_vals < 1 || val->type != type)
		return -EINVAL;

	size = SPA_POD_BODY_SIZE(val);
	vals = SPA_POD_BODY(val);

	switch (choice) {
	case SPA_CHOICE_None:
	case SPA_CHOICE_Enum:
		for (i = 0, v = vals; i < n_vals; i++, v = SPA_PTROFF(v, size, void)) {
			int diff = SPA_ABS(diff_value(impl, type, size, v, target));
			if (diff < res) {
				res = diff;
				best = v;
			}
		}
		if (fix) {
			if (best != NULL && best != vals)
				memcpy(vals, best, size);
			if (spa_pod_is_choice(&prop->value))
				SPA_POD_CHOICE_TYPE(&prop->value) = SPA_CHOICE_None;
		}
		break;
	default:
		return res;
	}
	return res;
}

static int calc_diff(struct impl *impl, struct spa_pod *param, struct dir *dir, bool fix)
{
	struct spa_pod_object *obj = (struct spa_pod_object*)param;
	struct spa_pod_prop *prop;
	struct spa_rectangle size;
	struct spa_fraction framerate;
	uint32_t format;
	int diff = 0;

	if (!dir->have_format)
		return -1;

	get_format(dir, &format, &size, &framerate);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_FORMAT_VIDEO_format:
			diff += diff_prop(impl, prop, SPA_TYPE_Id, &format, fix);
			break;
		case SPA_FORMAT_VIDEO_size:
			diff += diff_prop(impl, prop, SPA_TYPE_Rectangle, &size, fix);
			break;
		case SPA_FORMAT_VIDEO_framerate:
			diff += diff_prop(impl, prop, SPA_TYPE_Fraction, &framerate, fix);
			break;
		default:
			break;
		}
	}
	return diff;
}

static int all_formats(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[1];

	switch (index) {
	case 0:
		spa_pod_builder_push_object(b, &f[0],
				SPA_TYPE_OBJECT_Format, id);
		spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);
		add_video_formats(b, NULL, 0, port->is_dsp);
		*param = spa_pod_builder_pop(b, &f[0]);
		break;
	case 1:
		if (this->direction != port->direction)
			return 0;

		/* JPEG */
		spa_pod_builder_push_object(b, &f[0],
				SPA_TYPE_OBJECT_Format, id);
		spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_mjpg),
			0);
		*param = spa_pod_builder_pop(b, &f[0]);
		break;
	case 2:
		if (this->direction != port->direction)
			return 0;

		/* H264 */
		spa_pod_builder_push_object(b, &f[0],
				SPA_TYPE_OBJECT_Format, id);
		spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_h264),
			0);
		*param = spa_pod_builder_pop(b, &f[0]);
		break;
	default:
		return 0;
	}
	return 1;
}

static int port_param_enum_format(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	struct dir *other = &this->dir[SPA_DIRECTION_REVERSE(port->direction)];

	spa_log_debug(this->log, "%p %d %d %d %d", port, port->valid, this->direction, port->direction, index);
	if ((port->is_dsp || port->is_control) && index > 0)
		return 0;

	if (index == 0) {
		if (port->is_dsp) {
			struct spa_video_info_dsp info = SPA_VIDEO_INFO_DSP_INIT(
					.format = SPA_VIDEO_FORMAT_DSP_F32);
			*param = spa_format_video_dsp_build(b, id, &info);
			return 1;
		} else if (port->is_control) {
			*param = spa_pod_builder_add_object(b,
					SPA_TYPE_OBJECT_Format, id,
					SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
					SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
			return 1;
		} else if (other->have_format) {
			/* peer format */
			*param = spa_format_video_build(b, id, &other->format);
		}
		return 1;
	} else if (this->direction != port->direction) {
		struct port *oport = GET_PORT(this, SPA_DIRECTION_REVERSE(port->direction), port->id);
		if (oport != NULL && oport->valid &&
		    index - 1 < oport->n_peer_formats) {
			const struct spa_pod *p = oport->peer_formats[index-1];
			*param = transform_format(this, port, p, id, b);
		} else {
			return all_formats(this, port, id, index - 1 -
					(oport ? oport->n_peer_formats : 0), param, b);

		}
	} else if (index == 1) {
		const struct spa_pod *best = NULL;
		int best_diff = INT_MAX;
		uint32_t i;

		for (i = 0; i < port->n_peer_formats; i++) {
			const struct spa_pod *p = port->peer_formats[i];
			int diff = calc_diff(this, (struct spa_pod*)p, other, false);
			if (diff < 0)
				break;
			if (diff < best_diff) {
				best_diff = diff;
				best = p;
			}
		}
		if (best) {
			uint32_t offset = b->state.offset;
			struct spa_pod *p;
			spa_pod_builder_primitive(b, best);
			p = spa_pod_builder_deref(b, offset);
			calc_diff(this, p, other, true);
			*param = p;
		}
	} else {
		return all_formats(this, port, id, index - 2, param, b);
	}
	return 1;
}

static int port_param_format(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	if (!port->have_format)
		return -EIO;
	if (index != 0)
		return 0;

	if (port->is_dsp) {
		*param = spa_format_video_dsp_build(b, id, &port->format.info.dsp);
	} else if (port->is_control) {
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format,  id,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
	} else {
		*param = spa_format_video_build(b, id, &port->format);
	}
	return 1;
}

static int port_param_buffers(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	uint32_t size, min, max, def;
	struct port *other;

	if (!port->have_format)
		return -EIO;
	if (index != 0)
		return 0;

	if (port->is_dsp) {
		size = 1024 * 1024 * 16;
	} else {
		size = port->size;
	}

	other = GET_PORT(this, SPA_DIRECTION_REVERSE(port->direction), port->id);
	if (other->n_buffers > 0) {
		min = other->n_buffers;
	} else {
		min = 2;
	}
	max = MAX_BUFFERS;
	def = SPA_CLAMP(8u, min, MAX_BUFFERS);

	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_ParamBuffers, id,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(def, min, max),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(port->blocks),
		SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
						size, 16, INT32_MAX),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(
						port->stride, 0, INT32_MAX));
	return 1;
}

static int port_param_meta(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0:
		*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
		return 1;
	}
	return 0;
}

static int port_param_io(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0:
		*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
		return 1;
	}
	return 0;
}

static int port_param_latency(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0 ... 1:
		*param = spa_latency_build(b, id, &port->latency[index]);
		return 1;
	}
	return 0;
}

static int port_param_tag(struct impl *this, struct port *port, uint32_t id,
		uint32_t index, const struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (index) {
	case 0 ... 1:
		if (port->is_monitor)
			index = index ^ 1;
		*param = this->dir[index].tag;
		return 1;
	}
	return 0;
}

static int port_param_peer_enum_format(struct impl *this, struct port *port, uint32_t index,
		const struct spa_pod **param, struct spa_pod_builder *b)
{
	if (index >= port->n_peer_formats)
		return 0;

	*param = port->peer_formats[port->n_peer_formats - index];
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
	const struct spa_pod *param;
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

	param = NULL;
	switch (id) {
	case SPA_PARAM_EnumFormat:
		res = port_param_enum_format(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_Format:
		res = port_param_format(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_Buffers:
		res = port_param_buffers(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_Meta:
		res = port_param_meta(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_IO:
		res = port_param_io(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_Latency:
		res = port_param_latency(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_Tag:
		res = port_param_tag(this, port, id, result.index, &param, &b);
		break;
	case SPA_PARAM_PeerEnumFormat:
		res = port_param_peer_enum_format(this, port, result.index, &param, &b);
		break;
	default:
		return -ENOENT;
	}
	if (res <= 0)
		return res;

	if (param == NULL || spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	uint32_t i, j;

	spa_log_debug(this->log, "%p: clear buffers %p %d", this, port, port->n_buffers);
	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b = &port->buffers[i];
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			for (j = 0; j < b->buf->n_datas; j++) {
				if (b->datas[j]) {
					spa_log_debug(this->log, "%p: unmap buffer %d data %d %p",
							this, i, j, b->datas[j]);
					munmap(b->datas[j], b->buf->datas[j].maxsize);
					b->datas[j] = NULL;
				}
			}
			SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_MAPPED);
		}
	}
	port->n_buffers = 0;
	spa_list_init(&port->queue);
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
			}
		}
	}
	if (emit) {
		port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		port->params[IDX_Latency].user++;
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
		}
	}
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[IDX_Tag].user++;
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
		this->setup = false;
		this->fmt_passthrough = false;
		clear_buffers(this, port);
	} else {
		struct dir *dir = &this->dir[direction];
		struct dir *odir = &this->dir[SPA_DIRECTION_REVERSE(direction)];
		struct spa_video_info info = { 0 };

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
			dir->have_format = true;
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
			dir->have_format = true;
		}
		else {
			enum AVPixelFormat pix_fmt;

			if (info.media_type != SPA_MEDIA_TYPE_video) {
				spa_log_error(this->log, "unexpected types %d/%d",
						info.media_type, info.media_subtype);
				return -EINVAL;
			}
			if ((res = spa_format_video_parse(format, &info)) < 0) {
				spa_log_error(this->log, "can't parse format %s", spa_strerror(res));
				return res;
			}
			switch (info.media_subtype) {
			case SPA_MEDIA_SUBTYPE_raw:
			{
				int linesizes[4];

				pix_fmt = format_to_pix_fmt(info.info.raw.format);
				if (pix_fmt == AV_PIX_FMT_NONE)
					return -EINVAL;
				av_image_fill_linesizes(linesizes, pix_fmt, info.info.raw.size.width);
				port->stride = linesizes[0];
				port->blocks = 0;
				for (int i = 0; i < 4; i++) {
					dir->linesizes[i] = linesizes[i];
					if (linesizes[i])
						port->blocks++;
				}
				av_image_fill_plane_sizes(dir->plane_size,
						pix_fmt, info.info.raw.size.height,
						dir->linesizes);
				port->size = av_image_get_buffer_size(pix_fmt,
						info.info.raw.size.width,
						info.info.raw.size.height, this->max_align);

				break;
			}
			case SPA_MEDIA_SUBTYPE_h264:
				port->stride = 0;
				port->size = info.info.h264.size.width * info.info.h264.size.height;
				port->blocks = 1;
				break;
			case SPA_MEDIA_SUBTYPE_mjpg:
				port->stride = 0;
				port->size = info.info.mjpg.size.width * info.info.mjpg.size.height;
				port->blocks = 1;
				break;
			default:
				spa_log_error(this->log, "unsupported subtype %d", info.media_subtype);
				return -ENOTSUP;
			}
			dir->format = info;
			dir->have_format = true;
			if (odir->have_format) {
				this->fmt_passthrough =
					(memcmp(&odir->format, &dir->format, sizeof(dir->format)) == 0);
			}
		}
		port->format = info;
		port->have_format = true;
		this->setup = false;

		spa_log_debug(this->log, "%p: %d %d %d", this,
				port_id, port->stride, port->blocks);

		if (dir->have_format && odir->have_format)
			if ((res = setup_convert(this)) < 0)
				return res;
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
	return 0;
}


static int port_set_peer_enum_format(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *formats)
{
	struct impl *this = object;
	struct port *port, *oport;
	int res = 0;
	uint32_t i;
	enum spa_direction other = SPA_DIRECTION_REVERSE(direction);
	static uint32_t subtypes[] = {
		SPA_MEDIA_SUBTYPE_raw,
		SPA_MEDIA_SUBTYPE_mjpg,
		SPA_MEDIA_SUBTYPE_h264 };
	struct spa_peer_param_info info;
	void *state = NULL;

	spa_return_val_if_fail(spa_pod_is_object(formats), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	oport = GET_PORT(this, other, port_id);

	free(port->peer_formats);
	port->peer_formats = NULL;
	free(port->peer_format_pod);
	port->peer_format_pod = NULL;
	port->n_peer_formats = 0;

	if (formats) {
		uint32_t count = 0;
		port->peer_format_pod = spa_pod_copy(formats);

		for (i = 0; i < SPA_N_ELEMENTS(subtypes); i++) {
			state = NULL;
			while (spa_peer_param_parse(formats, &info, sizeof(info), &state) > 0) {
				uint32_t media_type, media_subtype;
				if (!spa_format_parse(info.param, &media_type, &media_subtype) ||
				    media_type != SPA_MEDIA_TYPE_video ||
				    media_subtype != subtypes[i])
					continue;
				count++;
			}
		}
		port->peer_formats = calloc(count, sizeof(struct spa_pod *));
		for (i = 0; i < SPA_N_ELEMENTS(subtypes); i++) {
			state = NULL;
			while (spa_peer_param_parse(port->peer_format_pod, &info, sizeof(info), &state) > 0) {
				uint32_t media_type, media_subtype;
				if (!spa_format_parse(info.param, &media_type, &media_subtype) ||
				    media_type != SPA_MEDIA_TYPE_video ||
				    media_subtype != subtypes[i])
					continue;
				port->peer_formats[port->n_peer_formats++] = info.param;
			}
		}
	}
	oport->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	oport->params[IDX_EnumFormat].user++;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[IDX_EnumFormat].user++;
	port->params[IDX_PeerEnumFormat].user++;
	return res;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set param port %d.%d %u",
			this, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Latency:
		res = port_set_latency(this, direction, port_id, flags, param);
		break;
	case SPA_PARAM_Tag:
		res = port_set_tag(this, direction, port_id, flags, param);
		break;
	case SPA_PARAM_Format:
		res = port_set_format(this, direction, port_id, flags, param);
		break;
	case SPA_PARAM_PeerEnumFormat:
		res = port_set_peer_enum_format(this, direction, port_id, flags, param);
		break;
		break;
	default:
		return -ENOENT;
	}
	emit_info(this, false);
	return res;
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

	spa_log_debug(this->log, "%p: use buffers %d on port %d:%d flags %08x",
			this, n_buffers, direction, port_id, flags);

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
		b->h = spa_buffer_find_meta_data(b->buf,
				SPA_META_Header, sizeof(struct spa_meta_header));

		if (n_datas != port->blocks) {
			spa_log_error(this->log, "%p: invalid blocks %d on buffer %d, expected %d",
					this, n_datas, i, port->blocks);
			return -EINVAL;
		}
		if (SPA_FLAG_IS_SET(flags, SPA_NODE_BUFFERS_FLAG_ALLOC)) {
			struct port *other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

			if (other->n_buffers <= 0)
				return -EIO;

			for (j = 0; j < n_datas; j++) {
				b->buf->datas[j] = other->buffers[i % other->n_buffers].buf->datas[j];
				b->datas[j] = other->buffers[i % other->n_buffers].datas[j];
				maxsize = SPA_MAX(maxsize, d[j].maxsize);
				spa_log_debug(this->log, "buffer %d: mem:%d passthrough:%p maxsize:%d",
						i, j, b->datas[j], d[j].maxsize);
			}
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
					SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
					spa_log_debug(this->log, "%p: mmap %d on buffer %d %d %p %p",
								this, j, i, d[j].type, data, b);
				}
				if (data != NULL && !SPA_IS_ALIGNED(data, this->max_align)) {
					spa_log_warn(this->log, "%p: memory %d on buffer %d not aligned",
							this, j, i);
				}
				b->datas[j] = data;
				spa_log_debug(this->log, "buffer %d: mem:%d data:%p maxsize:%d",
						i, j, data, d[j].maxsize);
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
			spa_loop_locked(this->data_loop, do_set_port_io, 0, NULL, 0, &d);
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

	if (this->fmt_passthrough) {
		dbuf = &out_port->buffers[input->buffer_id];
	} else if ((dbuf = peek_buffer(this, out_port)) == NULL) {
		spa_log_error(this->log, "%p: out of buffers", this);
		return -EPIPE;
	}

	spa_log_trace(this->log, "%d %p:%p %d %d %d", input->buffer_id, sbuf->buf->datas[0].chunk,
			dbuf->buf->datas[0].chunk, sbuf->buf->datas[0].chunk->size,
			sbuf->id, dbuf->id);

	/* do decoding */
	if (this->decoder.context) {
		this->decoder.packet->data = sbuf->datas[0];
		this->decoder.packet->size = sbuf->buf->datas[0].chunk->size;

		spa_log_trace(this->log, "decode %p:%d", this->decoder.packet->data,
				this->decoder.packet->size);

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
		in->size.width = f->width;
		in->size.height = f->height;
		for (uint32_t i = 0; i < 4; ++i) {
			datas[i] = f->data[i];
			strides[i] = f->linesize[i];
			sizes[i] = out->plane_size[i];
		}
	} else {
		f = this->decoder.frame;
		f->format = in->pix_fmt;
		f->width = in->size.width;
		f->height = in->size.height;
		for (uint32_t i = 0; i < sbuf->buf->n_datas; ++i) {
			datas[i] = f->data[i] = sbuf->datas[i];
			strides[i] = f->linesize[i] = sbuf->buf->datas[i].chunk->stride;
			sizes[i] = sbuf->buf->datas[i].chunk->size;
		}
	}

	/* do conversion */
	if (f->format != out->pix_fmt ||
	    f->width != (int)out->size.width ||
	    f->height != (int)out->size.height) {
		if (this->convert.context == NULL) {
			const AVPixFmtDescriptor *in_fmt = av_pix_fmt_desc_get(f->format);
			const AVPixFmtDescriptor *out_fmt = av_pix_fmt_desc_get(out->pix_fmt);
			this->convert.context = sws_getContext(
					f->width, f->height, f->format,
					out->size.width, out->size.height, out->pix_fmt,
					0, NULL, NULL, NULL);
			spa_log_info(this->log, "%p: using convert %dx%d:%s -> %dx%d:%s",
					this, f->width, f->height, in_fmt->name,
					out->size.width, out->size.height, out_fmt->name);
		}
		spa_log_trace(this->log, "convert");
		sws_scale_frame(this->convert.context, this->convert.frame, f);
		f = this->convert.frame;
		for (uint32_t i = 0; i < 4; ++i) {
			datas[i] = f->data[i];
			strides[i] = f->linesize[i];
			sizes[i] = out->plane_size[i];
		}
	}
	/* do encoding */
	if (this->encoder.context) {
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
		spa_log_trace(this->log, "encode %p %d", datas[0], sizes[0]);
	}

	/* write to output */
	for (uint32_t i = 0; i < dbuf->buf->n_datas; ++i) {
		if (SPA_FLAG_IS_SET(dbuf->buf->datas[i].flags, SPA_DATA_FLAG_DYNAMIC))
			dbuf->buf->datas[i].data = datas[i];
		else if (datas[i] && dbuf->datas[i] && dbuf->datas[i] != datas[i])
			memcpy(dbuf->datas[i], datas[i], sizes[i]);

		if (dbuf->buf->datas[i].chunk != sbuf->buf->datas[i].chunk) {
			dbuf->buf->datas[i].chunk->stride = strides[i];
			dbuf->buf->datas[i].chunk->size = sizes[i];
		}
		spa_log_trace(this->log, "out %u %u %p %d", dbuf->id, i,
				dbuf->buf->datas[i].data,
				dbuf->buf->datas[i].chunk->size);
	}
	dequeue_buffer(this, out_port, dbuf);

	if (sbuf->h && dbuf->h)
		*dbuf->h = *sbuf->h;

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

	free_decoder(this);
	free_encoder(this);
	av_frame_free(&this->decoder.frame);
	av_frame_free(&this->convert.frame);

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
		else if (spa_streq(k, "convert.direction")) {
			if (spa_streq(s, "output"))
				this->direction = SPA_DIRECTION_OUTPUT;
			else
				this->direction = SPA_DIRECTION_INPUT;
		}
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
