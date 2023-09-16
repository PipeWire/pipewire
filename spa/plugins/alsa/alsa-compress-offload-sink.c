/* Spa ALSA Compress-Offload sink */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Asymptotic Inc. */
/* SPDX-FileCopyrightText: Copyright @ 2023 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stddef.h>
#include <limits.h>
#include <linux/version.h>

#include <spa/monitor/device.h>
#include <spa/debug/types.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/support/system.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include "alsa.h"
#include "compress-offload-api.h"


/*
 * This creates a PipeWire sink node which uses the ALSA Compress-Offload API
 * for writing compressed data ike MP3, FLAC etc. to an DSP that can handle
 * such data directly.
 *
 * These show up under /dev/snd like "comprCxDx", as opposed to regular
 * ALSA PCM devices. This sink node still refers to those devices in
 * regular ALSA fashion as "hw:x,y" devices, where x = card number and
 * y = device number. For example, "hw:4,7" maps to /dev/snd/comprC4D7.
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.objects = [
 *  {   factory = adapter
 *      args = {
 *          factory.name   = "api.alsa.compress.offload.sink"
 *          node.name      = "Compress-Offload-Sink"
 *          node.description = "Audio sink for compressed audio"
 *          media.class    = "Audio/Sink"
 *          api.alsa.path  = "hw:0,3"
 *          node.param.PortConfig = {
 *              direction = Input
 *              mode = passthrough
 *          }
 *      }
 *  }
 *]
 *\endcode
 *
 * TODO:
 * - DLL for adjusting driver timer intervals to match the device timestamps in on_driver_timeout()
 * - Automatic loading using alsa-udev
 */


/* FLAC support has been present in kernel headers older than 5.5.
 * However, those older versions don't support FLAC decoding params. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)
#define COMPRESS_OFFLOAD_HAS_FLAC_DEC_PARAMS
#endif

/* Prior to kernel 5.7, WMA9 Pro/Lossless and WMA10 Lossless
 * codec profiles were missing.
 * As for ALAC and Monkey's Audio (APE), those are new in 5.7. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define COMPRESS_OFFLOAD_SUPPORTS_WMA9_PRO
#define COMPRESS_OFFLOAD_SUPPORTS_WMA9_LOSSLESS
#define COMPRESS_OFFLOAD_SUPPORTS_WMA10_LOSSLESS
#define COMPRESS_OFFLOAD_SUPPORTS_ALAC
#define COMPRESS_OFFLOAD_SUPPORTS_APE
#endif


#define CHECK_PORT(this, d, p)              (((d) == SPA_DIRECTION_INPUT) && ((p) == 0))

#define MAX_BUFFERS                         (32)

#define BUFFER_FLAG_AVAILABLE_FOR_NEW_DATA  (1 << 0)


/* Information about a buffer that got allocated by the PW graph. */
struct buffer {
	uint32_t id;
	uint32_t flags;
	struct spa_buffer *buf;
	struct spa_list link;
};


/* Node properties. These are accessible through SPA_PARAM_Props. */
struct props {
	/* The'"hw:<cardnr>:<devicenr>" device. */
	char device[128];
	/* These are the card and device numbers from the
	 * from the "hw:<cardnr>:<devicenr>" device.*/
	int card_nr;
	int device_nr;
	bool device_name_set;
};


/* Main sink node structure. */
struct impl {
	/* Basic states */

	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct props props;

	bool have_format;
	struct spa_audio_info current_audio_info;

	/* This is set to true when the SPA_NODE_COMMAND_Start is
	 * received, and set back to false when SPA_NODE_COMMAND_Pause
	 * or SPA_NODE_COMMAND_Suspend is received. */
	bool started;

	bool freewheel;

	/* SPA buffer states */

	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;
	struct spa_list queued_output_buffers;
	size_t offset_within_oldest_output_buffer;

	/* Driver and cycle specific states */

	int driver_timerfd;
	struct spa_source driver_timer_source;
	uint64_t next_driver_time;
	bool following;
	/* Duration and rate of one graph cycle.
	 * The duration equals the quantum size. */
	uint32_t cycle_duration;
	int cycle_rate;

	/* Node specific states */

	uint64_t node_info_all;
	struct spa_node_info node_info;
#define NODE_PropInfo        0
#define NODE_Props           1
#define NODE_IO              2
#define NODE_EnumPortConfig  3
#define N_NODE_PARAMS        4
	struct spa_param_info node_params[N_NODE_PARAMS];
	struct spa_io_clock *node_clock_io;
	struct spa_io_position *node_position_io;

	/* Port specific states */

	uint64_t port_info_all;
	struct spa_port_info port_info;
#define PORT_EnumFormat  0
#define PORT_Format      1
#define PORT_IO          2
#define PORT_Buffers     3
#define N_PORT_PARAMS    4
	struct spa_param_info port_params[N_PORT_PARAMS];
	struct spa_io_buffers *port_buffers_io;

	/* Compress-Offload specific states */

	struct compress_offload_api_context *device_context;
	struct snd_codec audio_codec_info;
	bool device_started;
	uint32_t min_fragment_size;
	uint32_t max_fragment_size;
	uint32_t min_num_fragments;
	uint32_t max_num_fragments;
	uint32_t configured_fragment_size;
	uint32_t configured_num_fragments;
	bool device_is_paused;
};



/* Compress-Offload device and audio codec functions */

static int init_audio_codec_info(struct impl *this, struct spa_audio_info *info, uint32_t *out_rate);
static int device_open(struct impl *this);
static void device_close(struct impl *this);
static int device_start(struct impl *this);
static int device_pause(struct impl *this);
static int device_resume(struct impl *this);
static int device_write(struct impl *this, const void *data, uint32_t size);

/* Driver timer functions */

static int set_driver_timeout(struct impl *this, uint64_t time);
static int configure_driver_timer(struct impl *this);
static int start_driver_timer(struct impl *this);
static void stop_driver_timer(struct impl *this);
static void on_driver_timeout(struct spa_source *source);
static inline void check_position_and_clock_config(struct impl *this);
static void reevaluate_following_state(struct impl *this);
static void reevaluate_freewheel_state(struct impl *this);

/* Miscellaneous functions */

static int parse_device(struct impl *this);
static void reset_props(struct props *props);
static void clear_buffers(struct impl *this);
static inline bool is_following(struct impl *this);
static int do_start(struct impl *this);
static void do_stop(struct impl *this);
static int write_queued_output_buffers(struct impl *this);
static const char * spa_command_to_string(const struct spa_command *command);

/* Node and port functions */

static void emit_node_info(struct impl *this, bool full);
static void emit_port_info(struct impl *this, bool full);
static int impl_node_add_listener(void *object,
                                  struct spa_hook *listener,
                                  const struct spa_node_events *events,
                                  void *data);
static int impl_node_set_callbacks(void *object,
                                   const struct spa_node_callbacks *callbacks,
                                   void *data);
static int impl_node_sync(void *object, int seq);
static int impl_node_enum_params(void *object, int seq,
                                 uint32_t id, uint32_t start, uint32_t num,
                                 const struct spa_pod *filter);
static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
		                       const struct spa_pod *param);
static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size);
static int impl_node_send_command(void *object, const struct spa_command *command);
static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		                      const struct spa_dict *props);
static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id);
static int port_enum_formats(struct impl *this, int seq, uint32_t start, uint32_t num,
                             const struct spa_pod *filter, struct spa_pod_builder *b);
static int impl_port_enum_params(void *object, int seq,
                                 enum spa_direction direction, uint32_t port_id,
                                 uint32_t id, uint32_t start, uint32_t num,
                                 const struct spa_pod *filter);
static int port_set_format(void *object,
                           enum spa_direction direction, uint32_t port_id,
                           uint32_t flags,
                           const struct spa_pod *format);
static int impl_port_set_param(void *object,
                               enum spa_direction direction, uint32_t port_id,
                               uint32_t id, uint32_t flags,
                               const struct spa_pod *param);
static int impl_port_use_buffers(void *object,
                                 enum spa_direction direction, uint32_t port_id,
                                 uint32_t flags,
                                 struct spa_buffer **buffers, uint32_t n_buffers);
static int impl_port_set_io(void *object,
                            enum spa_direction direction,
                            uint32_t port_id,
                            uint32_t id,
                            void *data, size_t size);
static int impl_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id);
static int impl_node_process(void *object);



/* Compress-Offload device and audio codec functions */

struct known_codec_info {
	uint32_t codec_id;
	uint32_t media_subtype;
	const char *name;
};

static struct known_codec_info known_codecs[] = {
	{ SND_AUDIOCODEC_VORBIS, SPA_MEDIA_SUBTYPE_vorbis, "Ogg Vorbis" },
	{ SND_AUDIOCODEC_MP3, SPA_MEDIA_SUBTYPE_mp3, "MP3" },
	{ SND_AUDIOCODEC_AAC, SPA_MEDIA_SUBTYPE_aac, "AAC" },
	{ SND_AUDIOCODEC_FLAC, SPA_MEDIA_SUBTYPE_flac, "FLAC" },
	{ SND_AUDIOCODEC_WMA, SPA_MEDIA_SUBTYPE_wma, "WMA" },
#ifdef COMPRESS_OFFLOAD_SUPPORTS_ALAC
	{ SND_AUDIOCODEC_ALAC, SPA_MEDIA_SUBTYPE_alac, "ALAC" },
#endif
#ifdef COMPRESS_OFFLOAD_SUPPORTS_APE
	{ SND_AUDIOCODEC_APE, SPA_MEDIA_SUBTYPE_ape, "Monkey's Audio (APE)" },
#endif
	{ SND_AUDIOCODEC_REAL, SPA_MEDIA_SUBTYPE_ra, "Real Audio" },
	{ SND_AUDIOCODEC_AMRWB, SPA_MEDIA_SUBTYPE_amr, "AMR wideband" },
	{ SND_AUDIOCODEC_AMR, SPA_MEDIA_SUBTYPE_amr, "AMR" },
};

static int init_audio_codec_info(struct impl *this, struct spa_audio_info *info, uint32_t *out_rate)
{
	struct snd_codec *codec;
	uint32_t channels, rate;
	const struct spa_type_info *media_subtype_info;

	media_subtype_info = spa_debug_type_find(spa_type_media_subtype, info->media_subtype);
	if (media_subtype_info == NULL) {
		spa_log_error(this->log, "%p: media subtype %d is unknown", this, info->media_subtype);
		return -ENOTSUP;
	}

	memset(&this->audio_codec_info, 0, sizeof(this->audio_codec_info));
	codec = &this->audio_codec_info;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_vorbis:
		codec->id = SND_AUDIOCODEC_VORBIS;
		rate = info->info.vorbis.rate;
		channels = info->info.vorbis.channels;
		spa_log_info(this->log, "%p: initialized codec info to Vorbis; rate: %"
		             PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;

	case SPA_MEDIA_SUBTYPE_mp3:
		codec->id = SND_AUDIOCODEC_MP3;
		rate = info->info.mp3.rate;
		channels = info->info.mp3.channels;
		spa_log_info(this->log, "%p: initialized codec info to MP3; rate: %"
		             PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;

	case SPA_MEDIA_SUBTYPE_aac:
		codec->id = SND_AUDIOCODEC_AAC;
		rate = info->info.aac.rate;
		channels = info->info.aac.channels;
		spa_log_info(this->log, "%p: initialized codec info to AAC; rate: %"
		             PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;

	case SPA_MEDIA_SUBTYPE_flac:
		codec->id = SND_AUDIOCODEC_FLAC;
#ifdef COMPRESS_OFFLOAD_HAS_FLAC_DEC_PARAMS
		/* The min/max block sizes are from the FLAC specification:
		 * https://xiph.org/flac/format.html#blocking
		 *
		 * The smallest valid frame possible is 11, which
		 * is why min_frame_size is set to this quantity.
		 *
		 * FFmpeg's flac.h specifies 8192 as the average frame size.
		 * tinycompress' fcplay uses 4x that amount as the max frame
		 * size to have enough headroom to be safe.
		 * We do the same here.
		 *
		 * sample_size is set to 0. According to the FLAC spec, this
		 * is OK to do if a STREAMINFO block was sent into the device
		 * (see: https://xiph.org/flac/format.html#frame_header), and
		 * we deal with full FLAC streams here, not just single frames. */
		codec->options.flac_d.min_blk_size = 16;
		codec->options.flac_d.max_blk_size = 65535;
		codec->options.flac_d.min_frame_size = 11;
		codec->options.flac_d.max_frame_size = 8192 * 4;
		codec->options.flac_d.sample_size = 0;
#endif
		rate = info->info.flac.rate;
		channels = info->info.flac.channels;
		spa_log_info(this->log, "%p: initialized codec info to FLAC; rate: %"
	                     PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;

	case SPA_MEDIA_SUBTYPE_wma: {
		const char *profile_name;
		codec->id = SND_AUDIOCODEC_WMA;
		/* WMA does not work with Compress-Offload
		 * if codec profile is not set. */
		switch (info->info.wma.profile) {
		case SPA_AUDIO_WMA_PROFILE_WMA9:
			codec->profile = SND_AUDIOPROFILE_WMA9;
			profile_name = "WMA9";
			break;
		case SPA_AUDIO_WMA_PROFILE_WMA10:
			codec->profile = SND_AUDIOPROFILE_WMA10;
			profile_name = "WMA10";
			break;
#ifdef COMPRESS_OFFLOAD_SUPPORTS_WMA9_PRO
		case SPA_AUDIO_WMA_PROFILE_WMA9_PRO:
			codec->profile = SND_AUDIOPROFILE_WMA9_PRO;
			profile_name = "WMA9 Pro";
			break;
#endif
#ifdef COMPRESS_OFFLOAD_SUPPORTS_WMA9_LOSSLESS
		case SPA_AUDIO_WMA_PROFILE_WMA9_LOSSLESS:
			codec->profile = SND_AUDIOPROFILE_WMA9_LOSSLESS;
			profile_name = "WMA9 Lossless";
			break;
#endif
#ifdef COMPRESS_OFFLOAD_SUPPORTS_WMA10_LOSSLESS
		case SPA_AUDIO_WMA_PROFILE_WMA10_LOSSLESS:
			codec->profile = SND_AUDIOPROFILE_WMA10_LOSSLESS;
			profile_name = "WMA10 Lossless";
			break;
#endif
		default:
			spa_log_error(this->log, "%p: Invalid WMA profile", this);
			return -EINVAL;
		}
		codec->bit_rate = info->info.wma.bitrate;
		codec->align = info->info.wma.block_align;
		rate = info->info.wma.rate;
		channels = info->info.wma.channels;
		spa_log_info(this->log, "%p: initialized codec info to WMA; rate: %"
		             PRIu32 "; channels: %" PRIu32 "; profile %s", this,
		             rate, channels, profile_name);
		break;
	}

#ifdef COMPRESS_OFFLOAD_SUPPORTS_ALAC
	case SPA_MEDIA_SUBTYPE_alac:
		codec->id = SND_AUDIOCODEC_ALAC;
		rate = info->info.alac.rate;
		channels = info->info.alac.channels;
		spa_log_info(this->log, "%p: initialized codec info to ALAC; rate: %"
		             PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;
#endif

#ifdef COMPRESS_OFFLOAD_SUPPORTS_APE
	case SPA_MEDIA_SUBTYPE_ape:
		codec->id = SND_AUDIOCODEC_APE;
		rate = info->info.ape.rate;
		channels = info->info.ape.channels;
		spa_log_info(this->log, "%p: initialized codec info to APE (Monkey's Audio);"
		             " rate: %" PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;
#endif

	case SPA_MEDIA_SUBTYPE_ra:
		codec->id = SND_AUDIOCODEC_REAL;
		rate = info->info.ra.rate;
		channels = info->info.ra.channels;
		spa_log_info(this->log, "%p: initialized codec info to Real Audio; rate: %"
		             PRIu32 "; channels: %" PRIu32, this, rate, channels);
		break;

	case SPA_MEDIA_SUBTYPE_amr:
		if (info->info.amr.band_mode == SPA_AUDIO_AMR_BAND_MODE_WB)
			codec->id = SND_AUDIOCODEC_AMRWB;
		else
			codec->id = SND_AUDIOCODEC_AMR;
		rate = info->info.amr.rate;
		channels = info->info.amr.channels;
		spa_log_info(this->log, "%p: initialized codec info to %s; rate: %"
		             PRIu32 "; channels: %" PRIu32, this,
		             (codec->id == SND_AUDIOCODEC_AMRWB) ? "AMR wideband" : "AMR",
		             rate, channels);
		break;

	default:
		spa_log_error(this->log, "%p: media subtype %s is not supported", this, media_subtype_info->name);
		return -ENOTSUP;
	}

	codec->ch_in = channels;
	codec->ch_out = channels;
	codec->sample_rate = rate;

	codec->rate_control = 0;
	codec->level = 0;
	codec->ch_mode = 0;
	codec->format = 0;

	*out_rate = rate;

	return 0;
}

static int device_open(struct impl *this)
{
	assert(this->device_context == NULL);

	spa_log_info(this->log, "%p: opening Compress-Offload device, card #%d device #%d",
	              this, this->props.card_nr, this->props.device_nr);

	this->device_context = compress_offload_api_open(this->props.card_nr, this->props.device_nr, this->log);
	if (this->device_context == NULL)
		return -errno;

	return 0;
}

static void device_close(struct impl *this)
{
	if (this->device_context == NULL)
		return;

	spa_log_info(this->log, "%p: closing Compress-Offload device, card #%d device #%d",
	              this, this->props.card_nr, this->props.device_nr);

	if (this->device_started)
		compress_offload_api_stop(this->device_context);

	compress_offload_api_close(this->device_context);

	this->device_context = NULL;
	this->device_started = false;
	this->device_is_paused = false;

	this->have_format = false;
}

static int device_start(struct impl *this)
{
	assert(this->device_context != NULL);

	if (compress_offload_api_start(this->device_context) < 0)
		return -errno;

	this->device_started = true;

	return 0;
}

static int device_pause(struct impl *this)
{
	/* device_pause() can sometimes be called when the device context is already
	 * gone. In particular, this can happen when the suspend command is received
	 * after the pause command. */
	if (this->device_context == NULL)
		return 0;

	if (this->device_is_paused)
		return 0;

	if (compress_offload_api_pause(this->device_context) < 0)
		return -errno;

	this->device_is_paused = true;

	return 0;
}

static int device_resume(struct impl *this)
{
	assert(this->device_context != NULL);

	if (!this->device_is_paused)
		return 0;

	if (compress_offload_api_resume(this->device_context) < 0)
		return -errno;

	this->device_is_paused = false;

	return 0;
}

static int device_write(struct impl *this, const void *data, uint32_t size)
{
	int res;
	uint32_t num_bytes_to_write;
	struct snd_compr_avail available_space;

	/* In here, try to write out as much data as possible,
	 * in a non-blocking manner, retaining the unwritten
	 * data for the next write call. */

	if (SPA_UNLIKELY((res = compress_offload_api_get_available_space(
	                 this->device_context, &available_space)) < 0))
		return res;

	/* We can only write data if there is at least enough space for one
	 * fragment's worth of data, or if the data we want to write is
	 * small (smaller than a fragment). The latter can happen when we
	 * are writing the last few bits of the compressed audio medium.
	 * When the former happens, we try to write as much data as we
	 * can, limited by the amount of space available in the device. */
	if ((available_space.avail < this->min_fragment_size) && (available_space.avail < size))
		return 0;

	num_bytes_to_write = SPA_MIN(size, available_space.avail);
	res = compress_offload_api_write(this->device_context, data, num_bytes_to_write);

	if (SPA_UNLIKELY(res < 0)) {
		if (res == -EBADFD)
			spa_log_debug(this->log, "%p: device is paused", this);
		else
			spa_log_error(this->log, "%p: write error: %s", this, spa_strerror(res));
		return res;
	}

	spa_log_trace_fp(this->log, "%p: wrote %d bytes; original request: %" PRIu32 "; adjusted "
	                 "for available space in device: %" PRIu32, this, res, size, num_bytes_to_write);

	if (SPA_UNLIKELY(((uint32_t)res) > num_bytes_to_write)) {
		spa_log_error(this->log, "%p: wrote more bytes than what was requested; "
		              "requested: %" PRId32 " wrote: %d", this, num_bytes_to_write, res);
		return -EIO;
	}

	return res;
}



/* Driver timer functions */

static int set_driver_timeout(struct impl *this, uint64_t time)
{
	struct itimerspec ts;

	ts.it_value.tv_sec = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(this->data_system, this->driver_timerfd,
	                           SPA_FD_TIMER_ABSTIME, &ts, NULL);

	return 0;
}

static int configure_driver_timer(struct impl *this)
{
	struct timespec now;
	int res;

	if ((res = spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now)) < 0) {
		spa_log_error(this->log, "%p: could not get time from monotonic sysclock: %s",
		              this, spa_strerror(res));
	    return res;
	}
	this->next_driver_time = SPA_TIMESPEC_TO_NSEC(&now);

	if (this->following)
		set_driver_timeout(this, 0);
	else
		set_driver_timeout(this, this->next_driver_time);

	return 0;
}

static int start_driver_timer(struct impl *this)
{
	int res;

	spa_log_debug(this->log, "%p: starting driver timer", this);

	this->driver_timer_source.func = on_driver_timeout;
	this->driver_timer_source.data = this;
	this->driver_timer_source.fd = this->driver_timerfd;
	this->driver_timer_source.mask = SPA_IO_IN;
	this->driver_timer_source.rmask = 0;

	spa_loop_add_source(this->data_loop, &this->driver_timer_source);

	if (SPA_UNLIKELY((res = configure_driver_timer(this)) < 0))
		return res;

	return 0;
}

static int do_remove_driver_timer_source(struct spa_loop *loop,
                                         bool async,
                                         uint32_t seq,
                                         const void *data,
                                         size_t size,
                                         void *user_data)
{
	struct impl *this = user_data;

	spa_loop_remove_source(this->data_loop, &this->driver_timer_source);
	set_driver_timeout(this, 0);

	return 0;
}

static void stop_driver_timer(struct impl *this)
{
	spa_log_debug(this->log, "%p: stopping driver timer", this);

	/* Perform the actual stop within
	 * the dataloop to avoid data races. */
	spa_loop_invoke(this->data_loop, do_remove_driver_timer_source, 0, NULL, 0, true, this);
}

static void on_driver_timeout(struct spa_source *source)
{
	struct impl *this = source->data;

	uint64_t expire, current_time;
	int res;

	if (SPA_LIKELY(this->started)) {
		if (SPA_UNLIKELY((res = spa_system_timerfd_read(this->data_system,
		                  this->driver_timerfd, &expire)) < 0)) {
			if (res != -EAGAIN)
				spa_log_warn(this->log, "%p: error reading from timerfd: %s",
				             this, spa_strerror(res));
			return;
		}
	}

	if (SPA_LIKELY(this->node_position_io != NULL)) {
		this->cycle_duration = this->node_position_io->clock.target_duration;
		this->cycle_rate = this->node_position_io->clock.target_rate.denom;
	} else {
		/* This can happen at the very beginning if node_position_io
		 * isn't passed to this node in time. */
		this->cycle_duration = 1024;
		this->cycle_rate = 48000;
	}

	current_time = this->next_driver_time;

	this->next_driver_time += ((uint64_t)(this->cycle_duration)) * 1000000000ULL / this->cycle_rate;
	if (this->node_clock_io != NULL) {
		this->node_clock_io->nsec = current_time;
		this->node_clock_io->rate = this->node_clock_io->target_rate;
		this->node_clock_io->position += this->node_clock_io->duration;
		this->node_clock_io->duration = this->cycle_duration;
		this->node_clock_io->delay = 0;
		this->node_clock_io->rate_diff = 1.0;
		this->node_clock_io->next_nsec = this->next_driver_time;
		spa_log_trace_fp(this->log, "%p: clock IO updated to: nsec %" PRIu64
			" pos %" PRIu64 " dur %" PRIu64 " next-nsec %" PRIu64, this,
			this->node_clock_io->nsec, this->node_clock_io->position,
			this->node_clock_io->duration, this->node_clock_io->next_nsec);
	}

	/* Adapt the graph cycle progression to the needs of the sink.
	 * If the sink still has data to output, don't advance. */
	if (spa_list_is_empty(&this->queued_output_buffers)) {
		struct spa_io_buffers *io = this->port_buffers_io;

		if (SPA_LIKELY(io != NULL)) {
			spa_log_trace_fp(this->log, "%p: ran out of buffers to output, "
			                 "need more; IO status: %d", this, io->status);
			io->status = SPA_STATUS_NEED_DATA;
			spa_node_call_ready(&this->callbacks, SPA_STATUS_NEED_DATA);
		} else {
			/* This should not happen. If it does, then there may be
			 * an error in when the timer is stopped. When it happens,
			 * do not schedule a next timeout. */
			spa_log_warn(this->log, "%p: buffers IO was set to NULL before "
			             "the driver timer was stopped", this);
			set_driver_timeout(this, 0);
			return;
		}
	} else {
		write_queued_output_buffers(this);
	}

	// TODO check for impossible timeouts (only relevant when taking device timestamps into account)

	set_driver_timeout(this, this->next_driver_time);
}

static inline void check_position_and_clock_config(struct impl *this)
{
	if (SPA_LIKELY(this->node_position_io != NULL)) {
		this->cycle_duration = this->node_position_io->clock.duration;
		this->cycle_rate = this->node_position_io->clock.rate.denom;
	} else {
		/* This can happen at the very beginning if node_position_io
		 * isn't passed to this node in time. */
		this->cycle_duration = 1024;
		this->cycle_rate = 48000;
	}
}

static int do_reevaluate_following_state(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;
	configure_driver_timer(this);
	return 0;
}

static void reevaluate_following_state(struct impl *this)
{
	bool following;

	if (!this->started)
		return;

	following = is_following(this);
	if (following != this->following) {
		spa_log_debug(this->log, "%p: following state changed: %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_invoke(this->data_loop, do_reevaluate_following_state, 0, NULL, 0, true, this);
	}
}

static void reevaluate_freewheel_state(struct impl *this)
{
	bool freewheel;

	if (!this->started)
		return;

	freewheel = (this->node_position_io != NULL) &&
		SPA_FLAG_IS_SET(this->node_position_io->clock.flags, SPA_IO_CLOCK_FLAG_FREEWHEEL);

	if (this->freewheel != freewheel) {
		spa_log_debug(this->log, "%p: freewheel state changed: %d->%d", this, this->freewheel, freewheel);
		this->freewheel = freewheel;
		if (freewheel)
			device_pause(this);
		else
			device_resume(this);
	}
}



/* Miscellaneous functions */

static int parse_device(struct impl *this)
{
	char *device;
	char *nextptr;
#define NUM_DEVICE_VALUES (2)
	long values[NUM_DEVICE_VALUES];
	int value_index;

	device = this->props.device;

	/* Valid devices always match the "hw:<cardnr>,<devicenr>" pattern. */

	if (strncmp(device, "hw:", 3) != 0) {
		spa_log_error(this->log, "%p: device \"%s\" does not begin with \"hw:\"", this, device);
		return -EINVAL;
	}

	nextptr = device + 3;
	for (value_index = 0; ; ++value_index) {
		const char *value_label;

		switch (value_index) {
		case 0: value_label = "card"; break;
		case 1: value_label = "device"; break;
		default: spa_assert_not_reached();
		}

		errno = 0;
		values[value_index] = strtol(nextptr, &nextptr, 10);
		if (errno != 0) {
			spa_log_error(this->log, "%p: device \"%s\" has invalid %s value",
			              this, device, value_label);
			return -EINVAL;
		}

		if (values[value_index] < 0) {
			spa_log_error(this->log, "%p: device \"%s\" has negative %s value",
			              this, device, value_label);
			return -EINVAL;
		}

		if (values[value_index] > INT_MAX) {
			spa_log_error(this->log, "%p: device \"%s\" has %s value larger than %d",
			              this, device, value_label, INT_MAX);
			return -EINVAL;
		}

		if (value_index >= (NUM_DEVICE_VALUES - 1))
			break;

		if ((*nextptr) != ',') {
			spa_log_error(this->log, "%p: expected ',' separator between numbers in "
			              "device \"%s\", got '%c'", this, device, *nextptr);
			return -EINVAL;
		}

		/* Skip the comma between the values. */
		nextptr++;
	}

	this->props.card_nr = values[0];
	this->props.device_nr = values[1];

	return 0;
}

static void reset_props(struct props *props)
{
	memset(props->device, 0, sizeof(props->device));
	props->card_nr = 0;
	props->device_nr = 0;
	props->device_name_set = false;
}

static void clear_buffers(struct impl *this)
{
	if (this->n_buffers > 0) {
		spa_log_debug(this->log, "%p: clearing buffers", this);
		spa_list_init(&this->queued_output_buffers);
		this->n_buffers = 0;
	}
}

static inline bool is_following(struct impl *this)
{
	return (this->node_position_io != NULL) &&
	       (this->node_clock_io != NULL) &&
	       (this->node_position_io->clock.id != this->node_clock_io->id);
}

static int do_start(struct impl *this)
{
	int res;

	if (this->started)
		return 0;

	this->following = is_following(this);
	spa_log_debug(this->log, "%p: starting output; starting as follower: %d",
	              this, this->following);

	if (SPA_UNLIKELY((res = start_driver_timer(this)) < 0))
		return res;

	this->started = true;

	/* Not starting the compress-offload device here right away.
	 * That's because we first need to give it at least one
	 * fragment's worth of data. Starting the device prior to
	 * that results in buffer underflows inside the device. */

	return 0;
}

static void do_stop(struct impl *this)
{
	if (!this->started)
		return;

	spa_log_debug(this->log, "%p: stopping output", this);

	device_pause(this);

	this->started = false;

	stop_driver_timer(this);
}

static int write_queued_output_buffers(struct impl *this)
{
	int res;
	uint32_t total_num_written_bytes;
	bool wrote_data = false;

	check_position_and_clock_config(this);

	/* In here, we write as much data as possible. The device may
	 * initially not have sufficient space, but it is possible
	 * that due to ongoing data consumption, it can accomodate
	 * for more data in a next attempt, hence the "again" label.
	 *
	 * If during the write attempts, only a portion of a chunk
	 * is written, we must keep track of the portion that hasn't
	 * been consumed yet. offset_within_oldest_output_buffer
	 * exists for this purpose. In this sink node, each SPA
	 * buffer has exactly one chunk, so when a chunk is fully
	 * consumed, the corresponding buffer is removed from the
	 * queued_output_buffers list, marked as available, and
	 * returned to the pool through spa_node_call_reuse_buffer(). */
again:
	total_num_written_bytes = 0;

	while (!spa_list_is_empty(&this->queued_output_buffers)) {
		struct buffer *b;
		struct spa_data *d;
		uint32_t chunk_start_offset, chunk_size, pending_data_size;
		bool reuse_buffer = false;

		b = spa_list_first(&this->queued_output_buffers, struct buffer, link);
		d = b->buf->datas;
		assert(b->buf->n_datas >= 1);

		chunk_start_offset = d[0].chunk->offset + this->offset_within_oldest_output_buffer;
		chunk_size = d[0].chunk->size;

		/* An empty chunk signals that the source is skipping this cycle. This
		 * is normal and necessary in cases when the compressed data frames are
		 * longer than the quantum size. The source then has to keep track of
		 * the excess lengths, and if these sum up to the length of a quantum,
		 * it sends a buffer with an empty chunk to compensate. If this is not
		 * done, there will eventually be an overflow, this sink will miss
		 * cycles, and audible errors will occur. */
		if (chunk_size != 0) {
			int num_written_bytes;

			pending_data_size = chunk_size - this->offset_within_oldest_output_buffer;

			chunk_start_offset = SPA_MIN(chunk_start_offset, d[0].maxsize);
			pending_data_size = SPA_MIN(pending_data_size, d[0].maxsize - chunk_start_offset);

			num_written_bytes = device_write(this, SPA_PTROFF(d[0].data, chunk_start_offset, void), pending_data_size);
			if (SPA_UNLIKELY(num_written_bytes < 0))
				return num_written_bytes;
			if (num_written_bytes == 0)
				break;

			this->offset_within_oldest_output_buffer += num_written_bytes;

			total_num_written_bytes += num_written_bytes;
			wrote_data = wrote_data || (num_written_bytes > 0);

			if (this->offset_within_oldest_output_buffer >= chunk_size) {
				spa_log_trace_fp(this->log, "%p: buffer with ID %u was fully written; reusing this buffer", this, b->id);
				reuse_buffer = true;
				this->offset_within_oldest_output_buffer = 0;
			}
		} else {
			spa_log_trace_fp(this->log, "%p: buffer with ID %u has empty chunk; reusing this buffer", this, b->id);
			reuse_buffer = true;
		}

		if (reuse_buffer) {
			spa_list_remove(&b->link);
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_AVAILABLE_FOR_NEW_DATA);
			this->port_buffers_io->buffer_id = b->id;
			spa_node_call_reuse_buffer(&this->callbacks, 0, b->id);
		}
	}

	if (!spa_list_is_empty(&this->queued_output_buffers) && (total_num_written_bytes > 0))
		goto again;

	/* We start the device only after having written data to avoid
	 * underruns due to an under-populated device ringbuffer. */
	if (wrote_data && !this->device_started) {
		spa_log_debug(this->log, "%p: starting device", this);
		if ((res = device_start(this)) < 0) {
			spa_log_error(this->log, "%p: starting device failed: %s", this, spa_strerror(res));
			return res;
		}
		this->device_started = true;
	}

	return 0;
}


static const char * spa_command_to_string(const struct spa_command *command)
{
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend: return "Suspend";
	case SPA_NODE_COMMAND_Pause: return "Pause";
	case SPA_NODE_COMMAND_Start: return "Start";
	case SPA_NODE_COMMAND_Enable: return "Enable";
	case SPA_NODE_COMMAND_Disable: return "Disable";
	case SPA_NODE_COMMAND_Flush: return "Flush";
	case SPA_NODE_COMMAND_Drain: return "Drain";
	case SPA_NODE_COMMAND_Marker: return "Marker";
	case SPA_NODE_COMMAND_ParamBegin: return "ParamBegin";
	case SPA_NODE_COMMAND_ParamEnd: return "ParamEnd";
	case SPA_NODE_COMMAND_RequestProcess: return "RequestProcess";
	default: return "<unknown>";
	}
}



/* Node and port functions */

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_DEVICE_API, "alsa" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Sink" },
	{ SPA_KEY_NODE_DRIVER, "true" },
	{ SPA_KEY_NODE_PAUSE_ON_IDLE, "true" },
};

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->node_info.change_mask : 0;

	if (full)
		this->node_info.change_mask = this->node_info_all;
	if (this->node_info.change_mask) {
		this->node_info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		spa_node_emit_info(&this->hooks, &this->node_info);
		this->node_info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->port_info.change_mask : 0;

	if (full)
		this->port_info.change_mask = this->port_info_all;
	if (this->port_info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
		                        SPA_DIRECTION_INPUT, 0, &this->port_info);
		this->port_info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object,
                                  struct spa_hook *listener,
                                  const struct spa_node_events *events,
                                  void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int impl_node_set_callbacks(void *object,
                                   const struct spa_node_callbacks *callbacks,
                                   void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_node_emit_result(&this->hooks, seq, 0, 0, NULL);

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
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,           SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_name,         SPA_POD_String(SPA_KEY_API_ALSA_PATH),
				SPA_PROP_INFO_description,  SPA_POD_String("The ALSA Compress-Offload device"),
				SPA_PROP_INFO_type,         SPA_POD_Stringn(p->device, sizeof(p->device)));
			break;
		default:
			return 0;
		}

		break;
	}

	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_device,       SPA_POD_Stringn(p->device, sizeof(p->device))
			);
			break;
		default:
			return 0;
		}

		break;
	}

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Clock),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_clock)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Position),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_position)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_EnumPortConfig:
	{
		switch (result.index) {
		case 0:
			/* Force ports to be configured to run in passthrough mode.
			 * This is essential when dealing with compressed data. */
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamPortConfig, id,
				SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
				SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_passthrough)
			);
			break;
		default:
			return 0;
		}

		break;
	}

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

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
		                       const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}

		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_device,       SPA_POD_OPT_Stringn(p->device, sizeof(p->device))
		);

		spa_log_debug(this->log, "%p: setting device name to \"%s\"", this, p->device);

		p->device_name_set = true;

		if ((res = parse_device(this)) < 0) {
			p->device_name_set = false;
			return res;
		}

		emit_node_info(this, false);

		break;
	}

	default:
		res = -ENOENT;
		break;
	}

	return res;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		spa_log_debug(this->log, "%p: got clock IO", this);
		this->node_clock_io = data;
		break;
	case SPA_IO_Position:
		spa_log_debug(this->log, "%p: got position IO", this);
		this->node_position_io = data;
		break;
	default:
		return -ENOENT;
	}

	reevaluate_following_state(this);
	reevaluate_freewheel_state(this);

	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: got new command: %s", this, spa_command_to_string(command));

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_ParamBegin:
		if (SPA_UNLIKELY((res = device_open(this)) < 0))
			return res;
		break;

	case SPA_NODE_COMMAND_ParamEnd:
		device_close(this);
		break;

	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if (SPA_UNLIKELY((res = do_start(this)) < 0))
			return res;

		break;

	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		do_stop(this);
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		                      const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(struct impl *this, int seq, uint32_t start, uint32_t num,
                             const struct spa_pod *filter, struct spa_pod_builder *b)
{
	bool device_started, device_opened;
	struct spa_result_node_params result;
	struct spa_pod *fmt;
	const struct known_codec_info *codec_info;
	uint32_t count = 0;
	int res;
	bool codec_supported;
	struct spa_audio_info info;

	device_opened = (this->device_context != NULL);
	device_started = this->device_started;

	spa_log_debug(this->log, "%p: about to enumerate supported codecs: "
	              "device opened: %d have configured format: %d device started: %d",
	              this, device_opened, this->have_format, device_started);

	if (!this->started && this->have_format) {
		spa_log_debug(this->log, "%p: closing device to reset configured format", this);
		device_close(this);
		device_opened = false;
	}

	if (!device_opened) {
		if ((res = device_open(this)) < 0)
			return res;
	}

	spa_zero(result);
	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

next:
	result.index = result.next++;

	if (result.index >= SPA_N_ELEMENTS(known_codecs))
		goto enum_end;

	codec_info = &(known_codecs[result.index]);

	codec_supported = compress_offload_api_supports_codec(this->device_context, codec_info->codec_id);

	spa_log_debug(this->log, "%p: codec %s supported: %s", this,
	              codec_info->name, codec_supported ? "yes" : "no");

	if (!codec_supported)
		goto next;

	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = codec_info->media_subtype;

	if ((fmt = spa_format_audio_build(b, SPA_PARAM_EnumFormat, &info)) == NULL) {
		res = -errno;
		spa_log_error(this->log, "%p: error while building enumerated audio info: %s",
		              this, spa_strerror(res));
		return res;
	}

	if (spa_pod_filter(b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

enum_end:
	res = 0;

	if (!device_opened)
		device_close(this);

	spa_log_debug(this->log, "%p: done enumerating supported codecs", this);

	return res;
}

static int impl_port_enum_params(void *object, int seq,
                                 enum spa_direction direction, uint32_t port_id,
                                 uint32_t id, uint32_t start, uint32_t num,
                                 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param = NULL;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		return port_enum_formats(this, seq, start, num, filter, &b);

	case SPA_PARAM_Format:
		if (!this->have_format) {
			spa_log_debug(this->log, "%p: attempted to enumerate current "
			              "format, but no current audio info set", this);
			return -EIO;
		}

		if (result.index > 0)
			return 0;

		spa_log_debug(this->log, "%p: current audio info is set; "
		              "enumerating currently set format", this);

		param = spa_format_audio_build(&b, id, &this->current_audio_info);
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

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			/* blocks is set to 1 since we don't have planar data */
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
				this->configured_fragment_size * this->configured_num_fragments,
				this->configured_fragment_size * this->configured_num_fragments,
				this->max_fragment_size * this->max_num_fragments),
			/* "stride" has no meaning when dealing with compressed data */
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(0));
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

static int port_set_format(void *object,
                           enum spa_direction direction, uint32_t port_id,
                           uint32_t flags,
                           const struct spa_pod *format)
{
	struct impl *this = object;
	int res;

	if (format == NULL) {
		if (!this->have_format)
			return 0;

		spa_log_debug(this->log, "%p: clearing format and closing device", this);
		device_close(this);
		clear_buffers(this);
	} else {
		struct spa_audio_info info = { 0 };
		uint32_t rate;
		const struct snd_compr_caps *compress_offload_caps;

		spa_log_debug(this->log, "%p: about to set format", this);

		if ((res = spa_format_audio_parse(format, &info)) < 0) {
			spa_log_error(this->log, "%p: error while parsing audio format: %s",
			              this, spa_strerror(res));
			return res;
		}

		if (this->device_context != NULL) {
			spa_log_debug(this->log, "%p: need to close device to be able to reopen it with new format", this);
			device_close(this);
		}

		if ((res = init_audio_codec_info(this, &info, &rate)) < 0)
			return res;

		if ((res = device_open(this)) < 0)
			return res;

		if (!compress_offload_api_supports_codec(this->device_context, this->audio_codec_info.id)) {
			spa_log_error(this->log, "%p: codec is not supported by the device", this);
			device_close(this);
			return -ENOTSUP;
		}

		if ((res = compress_offload_api_set_params(this->device_context, &(this->audio_codec_info), 0, 0)) < 0)
			return res;

		compress_offload_caps = compress_offload_api_get_caps(this->device_context);

		this->min_fragment_size = compress_offload_caps->min_fragment_size;
		this->max_fragment_size = compress_offload_caps->max_fragment_size;
		this->min_num_fragments = compress_offload_caps->min_fragments;
		this->max_num_fragments = compress_offload_caps->max_fragments;

		spa_log_debug(
			this->log,
			"%p: min/max fragment size: %" PRIu32 "/%" PRIu32 " min/max num fragments: %" PRIu32 "/%" PRIu32,
			this,
			this->min_fragment_size, this->max_fragment_size,
			this->min_num_fragments, this->max_num_fragments
		);

		compress_offload_api_get_fragment_config(this->device_context,
		                                         &(this->configured_fragment_size),
		                                         &(this->configured_num_fragments));

		spa_log_debug(
			this->log, "%p: configured fragment size: %" PRIu32 " configured num fragments: %" PRIu32,
			this,
			this->configured_fragment_size, this->configured_num_fragments
		);

		this->current_audio_info = info;
		this->have_format = true;
		this->port_info.rate = SPA_FRACTION(1, rate);
	}

	this->node_info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	this->node_info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	emit_node_info(this, false);

	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_RATE;
	this->port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;

	if (this->have_format) {
		this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
		this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	} else {
		this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
		this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	}

	emit_port_info(this, false);

	return 0;
}

static int impl_port_set_param(void *object,
                    enum spa_direction direction, uint32_t port_id,
                    uint32_t id, uint32_t flags,
                    const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		res = port_set_format(this, direction, port_id, flags, param);
		break;
	default:
		res = -ENOENT;
		break;
	}
	return res;
}

static int impl_port_use_buffers(void *object,
                                 enum spa_direction direction, uint32_t port_id,
                                 uint32_t flags,
                                 struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->n_buffers > 0) {
		spa_log_debug(this->log, "%p: %u buffers currently already in use; stopping device "
		              "to remove them before using new ones", this, this->n_buffers);
		do_stop(this);
		clear_buffers(this);
	}

	spa_log_debug(this->log, "%p: using a pool with %d buffer(s)", this, n_buffers);

	if (n_buffers > 0 && !this->have_format)
		return -EIO;
	if (n_buffers > MAX_BUFFERS)
		return -ENOSPC;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->id = i;
		b->flags = BUFFER_FLAG_AVAILABLE_FOR_NEW_DATA;
		b->buf = buffers[i];

		if (d[0].data == NULL) {
			spa_log_error(this->log, "%p: need mapped memory", this);
			return -EINVAL;
		}

		spa_log_debug(this->log, "%p: got buffer with ID %d bufptr %p data %p", this, i, b->buf, d[0].data);
	}

	this->n_buffers = n_buffers;

	return 0;
}

static int impl_port_set_io(void *object,
                            enum spa_direction direction,
                            uint32_t port_id,
                            uint32_t id,
                            void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_IO_Buffers:
		spa_log_debug(this->log, "%p: got buffers IO with data %p", this, data);
		this->port_buffers_io = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	struct spa_io_buffers *io;
	struct buffer *b;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	io = this->port_buffers_io;
	spa_return_val_if_fail(io != NULL, -EIO);

	/* Sinks aren't supposed to actually consume anything
	 * when the graph runs in freewheel mode. */
	if (this->node_position_io && this->node_position_io->clock.flags & SPA_IO_CLOCK_FLAG_FREEWHEEL) {
		io->status = SPA_STATUS_NEED_DATA;
		return SPA_STATUS_HAVE_DATA;
	}

	/* Add the incoming data if there is some. We place the data in
	 * a queue instead of just consuming it directly. This allows for
	 * adjusting driver cycles to the needs of the sink - if the sink
	 * already has data queued, it does not yet need to schedule a next
	 * cycle. See on_driver_timeout() for details. This is only relevnt
	 * if the sink is running as the graph's driver. */
	if ((io->status == SPA_STATUS_HAVE_DATA) && (io->buffer_id < this->n_buffers)) {
		b = &this->buffers[io->buffer_id];

		if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_AVAILABLE_FOR_NEW_DATA)) {
			spa_log_warn(this->log, "%p: buffer %u in use", this, io->buffer_id);
			io->status = -EINVAL;
			return -EINVAL;
		}

		if (this->device_is_paused) {
			spa_log_debug(this->log, "%p: resuming paused device", this);
			if ((res = device_resume(this)) < 0) {
				io->status = res;
				return SPA_STATUS_STOPPED;
			}
		}

		spa_log_trace_fp(this->log, "%p: queuing buffer %u", this, io->buffer_id);
		spa_list_append(&this->queued_output_buffers, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_AVAILABLE_FOR_NEW_DATA);
		/* This is essential to be able to hold back this buffer
		 * (which is because we queued it in a custom list for late
		 * consumption). By setting buffer_id to SPA_ID_INVALID,
		 * we essentially inform the graph that it must not attempt
		 * to return this buffer to the buffer pool. */
		io->buffer_id = SPA_ID_INVALID;

		if (SPA_UNLIKELY((res = write_queued_output_buffers(this)) < 0)) {
			io->status = res;
			return SPA_STATUS_STOPPED;
		}

		io->status = SPA_STATUS_OK;
	}

	return SPA_STATUS_HAVE_DATA;
}



/* SPA node information and init / clear procedures */

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
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_set_io = impl_port_set_io,
	.port_reuse_buffer = impl_port_reuse_buffer,
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

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	this = (struct impl *) handle;

	device_close(this);

	if (this->driver_timerfd > 0) {
		spa_system_close(this->data_system, this->driver_timerfd);
		this->driver_timerfd = -1;
	}

	spa_log_info(this->log, "%p: created Compress-Offload sink", this);

	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory,
	                        const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory, struct spa_handle *handle,
          const struct spa_dict *info, const struct spa_support *support, uint32_t n_support)
{
	struct impl *this;
	uint32_t i;
	int res = 0;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	/* A logger must always exist, otherwise something is very wrong. */
	assert(this->log != NULL);
	alsa_log_topic_init(this->log);

	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "%p: could not find a loop", this);
		res = -EINVAL;
		goto error;
	}

	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	if (this->data_system == NULL) {
		spa_log_error(this->log, "%p: could not find a data system", this);
		res = -EINVAL;
		goto error;
	}

	this->node.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Node,
		SPA_VERSION_NODE,
		&impl_node, this);

	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	this->have_format = false;

	this->started = false;

	this->freewheel = false;

	this->n_buffers = 0;
	spa_list_init(&this->queued_output_buffers);
	this->offset_within_oldest_output_buffer = 0;

	res = this->driver_timerfd = spa_system_timerfd_create(this->data_system, CLOCK_MONOTONIC,
	                                                       SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	if (SPA_UNLIKELY(res < 0)) {
		spa_log_error(this->log, "%p: could not create driver timerfd: %s", this, spa_strerror(res));
		goto error;
	}

	this->next_driver_time = 0;
	this->following = false;

	this->node_info_all = SPA_NODE_CHANGE_MASK_FLAGS |
	                      SPA_NODE_CHANGE_MASK_PROPS |
	                      SPA_NODE_CHANGE_MASK_PARAMS;
	this->node_info = SPA_NODE_INFO_INIT();
	this->node_info.max_input_ports = 1;
	this->node_info.flags = SPA_NODE_FLAG_RT |
	                        SPA_NODE_FLAG_IN_PORT_CONFIG |
	                        SPA_NODE_FLAG_NEED_CONFIGURE;
	this->node_params[NODE_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->node_params[NODE_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->node_params[NODE_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->node_params[NODE_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->node_info.params = this->node_params;
	this->node_info.n_params = N_NODE_PARAMS;
	this->node_clock_io = NULL;
	this->node_position_io = NULL;

	this->port_info_all = SPA_PORT_CHANGE_MASK_FLAGS |
	                      SPA_PORT_CHANGE_MASK_PARAMS;
	this->port_info = SPA_PORT_INFO_INIT();
	this->port_info.flags = SPA_PORT_FLAG_LIVE |
	                        SPA_PORT_FLAG_PHYSICAL |
	                        SPA_PORT_FLAG_TERMINAL;
	this->port_params[PORT_EnumFormat] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Format] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	this->port_params[PORT_IO] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	this->port_params[PORT_Buffers] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	this->port_info.params = this->port_params;
	this->port_info.n_params = N_PORT_PARAMS;
	this->port_buffers_io = NULL;

	this->device_context = NULL;
	this->device_started = false;
	memset(&this->audio_codec_info, 0, sizeof(this->audio_codec_info));
	this->device_is_paused = false;

	spa_log_info(this->log, "%p: initialized Compress-Offload sink", this);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, SPA_KEY_API_ALSA_PATH)) {
			snprintf(this->props.device, sizeof(this->props.device), "%s", s);
			if ((res = parse_device(this)) < 0)
				return res;
		}
	}

finish:
	return res;

error:
	impl_clear((struct spa_handle *)this);
	goto finish;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
                         const struct spa_interface_info **info, uint32_t *index)
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



/* Factory info */

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>, Carlos Rafael Giani <crg7475@mailbox.org>" },
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
