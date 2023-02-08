/* Spa ALSA Compress-Offload sink */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include <spa/monitor/device.h>
#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/system.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/json.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>

#include <sound/compress_params.h>
#include <tinycompress/tinycompress.h>

/*
 * This creates a PipeWire sink node which uses the tinycompress user space
 * library to use the ALSA Compress-Offload API for writing compressed data
 * like MP3, FLAC etc. to an DSP that can handle such data directly.
 *
 * These show up under /dev/snd like comprCxDx, as opposed to regular
 * ALSA PCM devices.
 *
 * root@dragonboard-845c:~# ls /dev/snd
 * by-path  comprC0D3  controlC0  pcmC0D0c  pcmC0D0p  pcmC0D1c  pcmC0D1p  pcmC0D2c  pcmC0D2p  timer
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.objects = [
 *  {   factory = spa-node-factory
 *      args = {
 *          factory.name   = api.alsa.compress.offload.sink
 *          node.name      = Compress-Offload-Sink
 *          media.class    = "Audio/Sink"
 *          api.alsa.path  = "hw:0,3"
 *      }
 *  }
 *]
 *\endcode
 *
 * TODO:
 * - Clocking
 * - Implement pause and resume
 * - Having a better wait mechanism
 * - Automatic loading using alsa-udev
 *
 */

#define NAME                "compress-offload-audio-sink"
#define DEFAULT_CHANNELS    2
#define DEFAULT_RATE        44100
#define MAX_BUFFERS         4
#define MAX_PORTS           1
#define MAX_CODECS          32 /* See include/sound/compress_params.h */

#define MIN_FRAGMENT_SIZE   (4 * 1024)
#define MAX_FRAGMENT_SIZE   (64 * 1024)
#define MIN_NUM_FRAGMENTS   (4)
#define MAX_NUM_FRAGMENTS   (8 * 4)

struct props {
	uint32_t channels;
	uint32_t rate;
	uint32_t pos[SPA_AUDIO_MAX_CHANNELS];
	char device[64];
};

static void reset_props(struct props *props)
{
	props->channels = 0;
	props->rate = 0;
}

struct buffer {
	uint32_t id;
	uint32_t flags;
	struct spa_buffer *outbuf;
};

struct impl;

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];

	struct spa_io_buffers *io;

	bool have_format;
	struct spa_audio_info current_format;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t written;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_log *log;
	struct props props;

	struct spa_node_info info;
	struct spa_param_info params[1];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	struct port port;

	unsigned int started_node:1;
	unsigned int started_compress:1;
	uint64_t info_all;
	uint32_t quantum_limit;

	struct compr_config compr_conf;
	struct snd_codec codec;
	struct compress *compress;

	int32_t codecs_supported[MAX_CODECS];
	uint32_t num_codecs;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS)

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_DEVICE_API, "alsa" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Sink" },
	{ SPA_KEY_NODE_DRIVER, "false" },
	{ SPA_KEY_NODE_PAUSE_ON_IDLE, "false" },
};

static const struct codec_id {
	uint32_t codec_id;
} codec_info[] = {
	{ SND_AUDIOCODEC_MP3, },
	{ SND_AUDIOCODEC_AAC, },
	{ SND_AUDIOCODEC_WMA, },
	{ SND_AUDIOCODEC_VORBIS, },
	{ SND_AUDIOCODEC_FLAC, },
	{ SND_AUDIOCODEC_ALAC, },
	{ SND_AUDIOCODEC_APE, },
	{ SND_AUDIOCODEC_REAL, },
	{ SND_AUDIOCODEC_AMR, },
	{ SND_AUDIOCODEC_AMRWB, },
};

static int
open_compress(struct impl *this)
{
	struct compress *compress;

	compress = compress_open_by_name(this->props.device, COMPRESS_IN, &this->compr_conf);
	if (!compress || !is_compress_ready(compress)) {
		spa_log_error(this->log, NAME " %p: Unable to open compress device", this);
		return -EINVAL;
	}

	this->compress = compress;

	compress_nonblock(this->compress, 1);

	return 0;
}

static int
write_compress(struct impl *this, void *buf, int32_t size)
{
	int32_t wrote;
	int32_t to_write = size;
	struct port *port = &this->port;

retry:
	wrote = compress_write(this->compress, buf, to_write);
	if (wrote < 0) {
		spa_log_error(this->log, NAME " %p: Error playing sample: %s",
				this, compress_get_error(this->compress));
		return wrote;
	}
	port->written += wrote;

	spa_log_debug(this->log, NAME " %p: We wrote %d, DSP accepted %d\n", this, size, wrote);

	if (wrote < to_write) {
		/*
		 * The choice of 20ms as the time to wait is
		 * completely arbitrary.
		 */
		compress_wait(this->compress, 20);
		buf = (uint8_t *)buf + wrote;
		to_write = to_write - wrote;
		goto retry;
	}

	/*
	 * One write has to happen before starting the compressed node. Calling
	 * compress_start before writing MIN_NUM_FRAGMENTS * MIN_FRAGMENT_SIZE
	 * will result in a distorted audio playback.
	 */
	if (!this->started_compress &&
			(port->written >= (MIN_FRAGMENT_SIZE * MIN_NUM_FRAGMENTS))) {
		compress_start(this->compress);
		this->started_compress = true;
	}

	return size;
}

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
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
				SPA_DIRECTION_INPUT, 0, &port->info);
		port->info.change_mask = old;
	}
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
	case SPA_PARAM_PortConfig:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_passthrough));
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

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int do_start(struct impl *this)
{
	if (this->started_node)
		return 0;

	spa_log_debug(this->log, "Open compressed device: %s", this->props.device);
	if (open_compress(this) < 0)
		return -EINVAL;

	this->started_node = true;
	this->started_compress = false;

	return 0;
}

static int do_drain(struct impl *this)
{
	if (!this->started_node)
		return 0;

	if (this->started_compress) {
		spa_log_debug(this->log, NAME " %p: Issuing drain command", this);
		compress_drain(this->compress);
		spa_log_debug(this->log, NAME " %p: Finished drain", this);
	}

	return 0;
}

static int do_stop(struct impl *this)
{
	if (!this->started_node)
		return 0;

	compress_stop(this->compress);
	compress_close(this->compress);
	spa_log_info(this->log, NAME " %p: Closed compress device", this);

	this->compress = NULL;
	this->started_node = false;
	this->started_compress = false;

	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	port = &this->port;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
	{
		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		do_start(this);
		break;
	}
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		do_drain(this);
		do_stop(this);
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

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
port_enum_formats(struct impl *this,
		  enum spa_direction direction, uint32_t port_id,
		  uint32_t index,
		  struct spa_pod **param,
		  struct spa_pod_builder *builder)
{
	struct spa_audio_info info;
	uint32_t codec;

	if (index >= this->num_codecs)
		return 0;

	codec = this->codecs_supported[index];

	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;

	switch (codec) {
	case SND_AUDIOCODEC_MP3:
		info.media_subtype = SPA_MEDIA_SUBTYPE_mp3;
		info.info.mp3.rate = this->props.rate;
		info.info.mp3.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_AAC:
		info.media_subtype = SPA_MEDIA_SUBTYPE_aac;
		info.info.aac.rate = this->props.rate;
		info.info.aac.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_WMA:
		info.media_subtype = SPA_MEDIA_SUBTYPE_wma;
		info.info.wma.rate = this->props.rate;
		info.info.wma.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_VORBIS:
		info.media_subtype = SPA_MEDIA_SUBTYPE_vorbis;
		info.info.vorbis.rate = this->props.rate;
		info.info.vorbis.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_FLAC:
		info.media_subtype = SPA_MEDIA_SUBTYPE_flac;
		info.info.flac.rate = this->props.rate;
		info.info.flac.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_ALAC:
		info.media_subtype = SPA_MEDIA_SUBTYPE_alac;
		info.info.alac.rate = this->props.rate;
		info.info.alac.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_APE:
		info.media_subtype = SPA_MEDIA_SUBTYPE_ape;
		info.info.ape.rate = this->props.rate;
		info.info.ape.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_REAL:
		info.media_subtype = SPA_MEDIA_SUBTYPE_ra;
		info.info.ra.rate = this->props.rate;
		info.info.ra.channels = this->props.channels;
		break;
	case SND_AUDIOCODEC_AMR:
		info.media_subtype = SPA_MEDIA_SUBTYPE_amr;
		info.info.amr.rate = this->props.rate;
		info.info.amr.channels = this->props.channels;
		info.info.amr.band_mode = SPA_AUDIO_AMR_BAND_MODE_NB;
		break;
	case SND_AUDIOCODEC_AMRWB:
		info.media_subtype = SPA_MEDIA_SUBTYPE_amr;
		info.info.amr.rate = this->props.rate;
		info.info.amr.channels = this->props.channels;
		info.info.amr.band_mode = SPA_AUDIO_AMR_BAND_MODE_WB;
		break;
	default:
		return -ENOTSUP;
	}
	if ((*param = spa_format_audio_build(builder, SPA_PARAM_EnumFormat, &info)) == NULL)
		return -errno;
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
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = &this->port;

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, direction, port_id,
						result.index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_format_audio_build(&b, id, &port->current_format);
		break;

	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(0),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							MIN_FRAGMENT_SIZE * MIN_NUM_FRAGMENTS,
							MIN_FRAGMENT_SIZE * MIN_NUM_FRAGMENTS,
							MAX_FRAGMENT_SIZE),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(0));
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
		spa_log_info(this->log, NAME " %p: clear buffers", this);
		port->n_buffers = 0;
		this->started_node = false;
	}
	return 0;
}

static int
compress_setup(struct impl *this, struct spa_audio_info *info, uint32_t *out_rate)
{
	struct compr_config *config;
	struct snd_codec *codec;
	uint32_t channels, rate;

	memset(&this->codec, 0, sizeof(this->codec));
	memset(&this->compr_conf, 0, sizeof(this->compr_conf));

	config = &this->compr_conf;
	codec = &this->codec;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_vorbis:
		codec->id = SND_AUDIOCODEC_VORBIS;
		rate = info->info.vorbis.rate;
		channels = info->info.vorbis.channels;
		break;
	case SPA_MEDIA_SUBTYPE_mp3:
		codec->id = SND_AUDIOCODEC_MP3;
		rate = info->info.mp3.rate;
		channels = info->info.mp3.channels;
		break;
	case SPA_MEDIA_SUBTYPE_aac:
		codec->id = SND_AUDIOCODEC_AAC;
		rate = info->info.aac.rate;
		channels = info->info.aac.channels;
		break;
	case SPA_MEDIA_SUBTYPE_flac:
		codec->id = SND_AUDIOCODEC_FLAC;
		/*
		 * Taken from the fcplay utility in tinycompress. Required for
		 * FLAC to work.
		 */
		codec->options.flac_d.sample_size = 16;
		codec->options.flac_d.min_blk_size = 16;
		codec->options.flac_d.max_blk_size = 65535;
		codec->options.flac_d.min_frame_size = 11;
		codec->options.flac_d.max_frame_size = 8192 * 4;
		rate = info->info.flac.rate;
		channels = info->info.flac.channels;
		break;
	case SPA_MEDIA_SUBTYPE_wma:
		codec->id = SND_AUDIOCODEC_WMA;
		/*
		 * WMA does not work with Compress-Offload if codec profile
		 * is not set.
		 */
		switch (info->info.wma.profile) {
		case SPA_AUDIO_WMA_PROFILE_WMA9:
			codec->profile = SND_AUDIOPROFILE_WMA9;
			break;
		case SPA_AUDIO_WMA_PROFILE_WMA9_PRO:
			codec->profile = SND_AUDIOPROFILE_WMA9_PRO;
			break;
		case SPA_AUDIO_WMA_PROFILE_WMA9_LOSSLESS:
			codec->profile = SND_AUDIOPROFILE_WMA9_LOSSLESS;
			break;
		case SPA_AUDIO_WMA_PROFILE_WMA10:
			codec->profile = SND_AUDIOPROFILE_WMA10;
			break;
		case SPA_AUDIO_WMA_PROFILE_WMA10_LOSSLESS:
			codec->profile = SND_AUDIOPROFILE_WMA10_LOSSLESS;
			break;
		default:
			spa_log_error(this->log, NAME " %p: Invalid WMA codec profile", this);
			return -EINVAL;
		}
		codec->bit_rate = info->info.wma.bitrate;
		codec->align = info->info.wma.block_align;
		rate = info->info.wma.rate;
		channels = info->info.wma.channels;
		break;
	case SPA_MEDIA_SUBTYPE_alac:
		codec->id = SND_AUDIOCODEC_ALAC;
		rate = info->info.alac.rate;
		channels = info->info.alac.channels;
		break;
	case SPA_MEDIA_SUBTYPE_ape:
		codec->id = SND_AUDIOCODEC_APE;
		rate = info->info.ape.rate;
		channels = info->info.ape.channels;
		break;
	case SPA_MEDIA_SUBTYPE_ra:
		codec->id = SND_AUDIOCODEC_REAL;
		rate = info->info.ra.rate;
		channels = info->info.ra.channels;
		break;
	case SPA_MEDIA_SUBTYPE_amr:
		if (info->info.amr.band_mode == SPA_AUDIO_AMR_BAND_MODE_WB)
			codec->id = SND_AUDIOCODEC_AMRWB;
		else
			codec->id = SND_AUDIOCODEC_AMR;
		rate = info->info.amr.rate;
		channels = info->info.amr.channels;
		break;
		break;
	default:
		return -ENOTSUP;
	}

	codec->ch_in = channels;
	codec->ch_out = channels;
	codec->sample_rate = rate;
	*out_rate = rate;

	codec->rate_control = 0;
	codec->level = 0;
	codec->ch_mode = 0;
	codec->format = 0;

	spa_log_info(this->log, NAME " %p: Codec info, profile: %d align: %d rate: %d bitrate: %d",
			this, codec->profile, codec->align, codec->sample_rate, codec->bit_rate);

	if (!is_codec_supported_by_name(this->props.device, 0, codec)) {
		spa_log_error(this->log, NAME " %p: Requested codec is not supported by DSP", this);
		return -EINVAL;
	}

	config->codec = codec;
	config->fragment_size = MIN_FRAGMENT_SIZE;
	config->fragments = MIN_NUM_FRAGMENTS;

	return 0;
}

static int
port_set_format(struct impl *this,
		enum spa_direction direction,
		uint32_t port_id,
		uint32_t flags,
		const struct spa_pod *format)
{
	int res;
	struct port *port = &this->port;

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { 0 };
		uint32_t rate;

		if ((res = spa_format_audio_parse(format, &info)) < 0) {
			spa_log_error(this->log, NAME " %p: format parse error: %s", this,
					spa_strerror(res));
			return res;
		}

		if ((res = compress_setup(this, &info, &rate)) < 0) {
			spa_log_error(this->log, NAME " %p: can't setup compress: %s",
					this, spa_strerror(res));
			return res;
		}

		port->current_format = info;
		port->have_format = true;
		port->info.rate = SPA_FRACTION(1, rate);
	}

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	emit_node_info(this, false);

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;

	if (port->have_format) {
		port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
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

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
	return 0;
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

	port = &this->port;

	if (!port->have_format)
		return -EIO;

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->id = i;
		b->flags = 0;
		b->outbuf = buffers[i];

		if (d[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
	}
	port->n_buffers = n_buffers;

	return 0;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;
	struct port *port;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = &this->port;

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct port *port;
	struct spa_io_buffers *io;
	struct buffer *b;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	port = &this->port;

	io = port->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status != SPA_STATUS_HAVE_DATA)
		return io->status;

	if (io->buffer_id >= port->n_buffers) {
		io->status = -EINVAL;
		return io->status;
	}

	b = &port->buffers[io->buffer_id];

	for (i = 0; i < b->outbuf->n_datas; i++) {
		int32_t offs, size;
		int32_t wrote;
		void *buf;

		struct spa_data *d = b->outbuf->datas;
		d = b->outbuf->datas;

		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->maxsize - offs, d->chunk->size);
		buf = SPA_PTROFF(d[0].data, offs, void);

		wrote = write_compress(this, buf, size);
		if (wrote < 0) {
			spa_log_error(this->log, NAME " %p: Error playing sample: %s",
					this, compress_get_error(this->compress));
			io->status = wrote;
			return SPA_STATUS_STOPPED;
		}
	}

	io->status = SPA_STATUS_OK;

	return SPA_STATUS_HAVE_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_set_io = impl_node_port_set_io,
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
	const char *str;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all |= SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = MAX_PORTS;
	this->info.max_output_ports = 0;
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_IN_PORT_CONFIG |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->info.params = this->params;
	this->info.n_params = 1;
	reset_props(&this->props);

	port = &this->port;
	port->info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF |
			SPA_PORT_FLAG_LIVE |
			SPA_PORT_FLAG_PHYSICAL |
			SPA_PORT_FLAG_TERMINAL;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	port->info.params = port->params;
	port->info.n_params = 4;
	port->written = 0;

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "clock.quantum-limit")) {
			spa_atou32(s, &this->quantum_limit, 0);
		} else if (spa_streq(k, SPA_KEY_AUDIO_CHANNELS)) {
			this->props.channels = atoi(s);
		} else if (spa_streq(k, SPA_KEY_AUDIO_RATE)) {
			this->props.rate = atoi(s);
		}
	}

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_ALSA_PATH))) {
		if ((str[0] == 'h') || (str[1] == 'w') || (str[2] == ':')) {
			snprintf(this->props.device, sizeof(this->props.device), "%s", str);
		} else {
			spa_log_error(this->log, NAME " %p: Invalid Compress-Offload hw %s", this, str);
			return -EINVAL;
		}
	} else {
		spa_log_error(this->log, NAME " %p: Invalid compress hw", this);
		return -EINVAL;
	}

	/*
	 * TODO:
	 *
	 * Move this to use new compress_get_supported_codecs_by_name API once
	 * merged upstream.
	 *
	 * Right now, we pretend all codecs are supported and then error out
	 * at runtime in port_set_format during compress_setup if not
	 * supported.
	 */
	this->num_codecs = SPA_N_ELEMENTS (codec_info);
	for (i = 0; i < this->num_codecs; i++) {
		this->codecs_supported[i] = codec_info[i].codec_id;
	}

	spa_log_info(this->log, NAME " %p: Initialized Compress-Offload sink %s",
			this, this->props.device);

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

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Play compressed audio (like MP3 or AAC) with the ALSA Compress-Offload API" },
	{ SPA_KEY_FACTORY_USAGE, "["SPA_KEY_API_ALSA_PATH"=<path>]" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_alsa_compress_offload_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALSA_COMPRESS_OFFLOAD_SINK,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
