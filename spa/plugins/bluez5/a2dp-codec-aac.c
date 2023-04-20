/* Spa A2DP AAC codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

#include <spa/param/audio/format.h>
#include <spa/utils/dict.h>

#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>

#include "rtp.h"
#include "media-codecs.h"

static struct spa_log *log;
static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.codecs.aac");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define DEFAULT_AAC_BITRATE	320000
#define MIN_AAC_BITRATE		64000

struct props {
	int bitratemode;
};

struct impl {
	HANDLE_AACENCODER aacenc;
	HANDLE_AACDECODER aacdec;

	struct rtp_header *header;

	size_t mtu;
	int codesize;

	int max_bitrate;
	int cur_bitrate;

	uint32_t rate;
	uint32_t channels;
	int samplesize;
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	static const a2dp_aac_t a2dp_aac = {
		.object_type =
			/* NOTE: AAC Long Term Prediction and AAC Scalable are
			 *       not supported by the FDK-AAC library. */
			AAC_OBJECT_TYPE_MPEG2_AAC_LC |
			AAC_OBJECT_TYPE_MPEG4_AAC_LC,
		AAC_INIT_FREQUENCY(
			AAC_SAMPLING_FREQ_8000 |
			AAC_SAMPLING_FREQ_11025 |
			AAC_SAMPLING_FREQ_12000 |
			AAC_SAMPLING_FREQ_16000 |
			AAC_SAMPLING_FREQ_22050 |
			AAC_SAMPLING_FREQ_24000 |
			AAC_SAMPLING_FREQ_32000 |
			AAC_SAMPLING_FREQ_44100 |
			AAC_SAMPLING_FREQ_48000 |
			AAC_SAMPLING_FREQ_64000 |
			AAC_SAMPLING_FREQ_88200 |
			AAC_SAMPLING_FREQ_96000)
		.channels =
			AAC_CHANNELS_1 |
			AAC_CHANNELS_2,
		.vbr = 1,
		AAC_INIT_BITRATE(DEFAULT_AAC_BITRATE)
	};

	memcpy(caps, &a2dp_aac, sizeof(a2dp_aac));
	return sizeof(a2dp_aac);
}

static const struct media_codec_config
aac_frequencies[] = {
	{ AAC_SAMPLING_FREQ_48000, 48000, 11 },
	{ AAC_SAMPLING_FREQ_44100, 44100, 10 },
	{ AAC_SAMPLING_FREQ_96000, 96000, 9 },
	{ AAC_SAMPLING_FREQ_88200, 88200, 8 },
	{ AAC_SAMPLING_FREQ_64000, 64000, 7 },
	{ AAC_SAMPLING_FREQ_32000, 32000, 6 },
	{ AAC_SAMPLING_FREQ_24000, 24000, 5 },
	{ AAC_SAMPLING_FREQ_22050, 22050, 4 },
	{ AAC_SAMPLING_FREQ_16000, 16000, 3 },
	{ AAC_SAMPLING_FREQ_12000, 12000, 2 },
	{ AAC_SAMPLING_FREQ_11025, 11025, 1 },
	{ AAC_SAMPLING_FREQ_8000,  8000,  0 },
};

static const struct media_codec_config
aac_channel_modes[] = {
	{ AAC_CHANNELS_2, 2, 1 },
	{ AAC_CHANNELS_1, 1, 0 },
};

static int get_valid_aac_bitrate(a2dp_aac_t *conf)
{
	if (AAC_GET_BITRATE(*conf) < MIN_AAC_BITRATE) {
		/* Unknown (0) or bogus bitrate */
		return DEFAULT_AAC_BITRATE;
	} else {
		return SPA_MIN(AAC_GET_BITRATE(*conf), DEFAULT_AAC_BITRATE);
	}
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_aac_t conf;
	int i;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	conf = *(a2dp_aac_t*)caps;

	if (conf.object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
		return -ENOTSUP;  /* Not supported by FDK-AAC */
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
		return -ENOTSUP;  /* Not supported by FDK-AAC */
	else
		return -ENOTSUP;

	if ((i = media_codec_select_config(aac_frequencies,
					  SPA_N_ELEMENTS(aac_frequencies),
					  AAC_GET_FREQUENCY(conf),
				    	  info ? info->rate : A2DP_CODEC_DEFAULT_RATE
				    	  )) < 0)
		return -ENOTSUP;
	AAC_SET_FREQUENCY(conf, aac_frequencies[i].config);

	if ((i = media_codec_select_config(aac_channel_modes,
					  SPA_N_ELEMENTS(aac_channel_modes),
					  conf.channels,
				    	  info ? info->channels : A2DP_CODEC_DEFAULT_CHANNELS
				    	  )) < 0)
		return -ENOTSUP;
	conf.channels = aac_channel_modes[i].config;

	AAC_SET_BITRATE(conf, get_valid_aac_bitrate(&conf));

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_aac_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t i = 0;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	SPA_FOR_EACH_ELEMENT_VAR(aac_frequencies, f) {
		if (AAC_GET_FREQUENCY(conf) & f->config) {
			if (i++ == 0)
				spa_pod_builder_int(b, f->value);
			spa_pod_builder_int(b, f->value);
		}
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (i == 0)
		return -EINVAL;

	if (SPA_FLAG_IS_SET(conf.channels, AAC_CHANNELS_1 | AAC_CHANNELS_2)) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 2),
				0);
	} else if (conf.channels & AAC_CHANNELS_1) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 1, position),
				0);
	} else if (conf.channels & AAC_CHANNELS_2) {
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 2, position),
				0);
	} else
		return -EINVAL;

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	a2dp_aac_t conf;
	size_t j;

	if (caps == NULL || caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S16;

	/*
	 * A2DP v1.3.2, 4.5.2: only one bit shall be set in bitfields.
	 * However, there is a report (#1342) of device setting multiple
	 * bits for AAC object type. It's not clear if this was due to
	 * a BlueZ bug, but we can be lax here and below in codec_init.
	 */
	if (!(conf.object_type & (AAC_OBJECT_TYPE_MPEG2_AAC_LC |
					AAC_OBJECT_TYPE_MPEG4_AAC_LC)))
		return -EINVAL;
	j = 0;
	SPA_FOR_EACH_ELEMENT_VAR(aac_frequencies, f) {
		if (AAC_GET_FREQUENCY(conf) & f->config) {
			info->info.raw.rate = f->value;
			j++;
			break;
		}
	}
	if (j == 0)
		return -EINVAL;

	if (conf.channels & AAC_CHANNELS_2) {
		info->info.raw.channels = 2;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
		info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	} else if (conf.channels & AAC_CHANNELS_1) {
		info->info.raw.channels = 1;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		return -EINVAL;
	}

	return 0;
}

static void *codec_init_props(const struct media_codec *codec, uint32_t flags, const struct spa_dict *settings)
{
	struct props *p = calloc(1, sizeof(struct props));
	const char *str;

	if (p == NULL)
		return NULL;

	if (settings == NULL || (str = spa_dict_lookup(settings, "bluez5.a2dp.aac.bitratemode")) == NULL)
		str = "0";

	p->bitratemode = SPA_CLAMP(atoi(str), 0, 5);
	return p;
}

static void codec_clear_props(void *props)
{
	free(props);
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	a2dp_aac_t *conf = config;
	struct props *p = props;
	UINT bitratemode;
	int res;

	this = calloc(1, sizeof(struct impl));
	if (this == NULL) {
		res = -errno;
		goto error;
	}
	this->mtu = mtu;
	this->rate = info->info.raw.rate;
	this->channels = info->info.raw.channels;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S16) {
		res = -EINVAL;
		goto error;
	}
	this->samplesize = 2;

	bitratemode = p ? p->bitratemode : 0;

	res = aacEncOpen(&this->aacenc, 0, this->channels);
	if (res != AACENC_OK)
		goto error;

	if (!(conf->object_type & (AAC_OBJECT_TYPE_MPEG2_AAC_LC |
					AAC_OBJECT_TYPE_MPEG4_AAC_LC))) {
		res = -EINVAL;
		goto error;
	}

	res = aacEncoder_SetParam(this->aacenc, AACENC_AOT, AOT_AAC_LC);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_SAMPLERATE, this->rate);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_CHANNELMODE, this->channels);
	if (res != AACENC_OK)
		goto error;

	if (conf->vbr) {
		res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATEMODE,
				bitratemode);
		if (res != AACENC_OK)
			goto error;
	}

	res = aacEncoder_SetParam(this->aacenc, AACENC_AUDIOMUXVER, 2);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_SIGNALING_MODE, 1);
	if (res != AACENC_OK)
		goto error;

	// Fragmentation is not implemented yet,
	// so make sure every encoded AAC frame fits in (mtu - header)
	this->max_bitrate = ((this->mtu - sizeof(struct rtp_header)) * 8 * this->rate) / 1024;
	this->max_bitrate = SPA_MIN(this->max_bitrate, get_valid_aac_bitrate(conf));
	this->cur_bitrate = this->max_bitrate;

	res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATE, this->cur_bitrate);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_PEAK_BITRATE, this->max_bitrate);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_TRANSMUX, TT_MP4_LATM_MCP1);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_HEADER_PERIOD, 1);
	if (res != AACENC_OK)
		goto error;

	res = aacEncoder_SetParam(this->aacenc, AACENC_AFTERBURNER, 1);
	if (res != AACENC_OK)
		goto error;

	res = aacEncEncode(this->aacenc, NULL, NULL, NULL, NULL);
	if (res != AACENC_OK)
		goto error;

	AACENC_InfoStruct enc_info = {};
	res = aacEncInfo(this->aacenc, &enc_info);
	if (res != AACENC_OK)
		goto error;

	this->codesize = enc_info.frameLength * this->channels * this->samplesize;

	this->aacdec = aacDecoder_Open(TT_MP4_LATM_MCP1, 1);
	if (!this->aacdec) {
		res = -EINVAL;
		goto error;
	}

#ifdef AACDECODER_LIB_VL0
	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_MIN_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set min output channels: 0x%04X", res);
		goto error;
	}

	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_MAX_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set max output channels: 0x%04X", res);
		goto error;
	}
#else
	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set output channels: 0x%04X", res);
		goto error;
	}
#endif

	return this;

error:
	if (this && this->aacenc)
		aacEncClose(&this->aacenc);
	if (this && this->aacdec)
		aacDecoder_Close(this->aacdec);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	if (this->aacenc)
		aacEncClose(&this->aacenc);
	if (this->aacdec)
		aacDecoder_Close(this->aacdec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;

	this->header = (struct rtp_header *)dst;
	memset(this->header, 0, sizeof(struct rtp_header));

	this->header->v = 2;
	this->header->pt = 96;
	this->header->sequence_number = htons(seqnum);
	this->header->timestamp = htonl(timestamp);
	this->header->ssrc = htonl(1);
	return sizeof(struct rtp_header);
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int res;

	void *in_bufs[] = {(void *) src};
	int in_buf_ids[] = {IN_AUDIO_DATA};
	int in_buf_sizes[] = {src_size};
	int in_buf_el_sizes[] = {this->samplesize};
	AACENC_BufDesc in_buf_desc = {
		.numBufs = 1,
		.bufs = in_bufs,
		.bufferIdentifiers = in_buf_ids,
		.bufSizes = in_buf_sizes,
		.bufElSizes = in_buf_el_sizes,
	};
	AACENC_InArgs in_args = {
		.numInSamples = src_size / this->samplesize,
	};

	void *out_bufs[] = {dst};
	int out_buf_ids[] = {OUT_BITSTREAM_DATA};
	int out_buf_sizes[] = {dst_size};
	int out_buf_el_sizes[] = {this->samplesize};
	AACENC_BufDesc out_buf_desc = {
		.numBufs = 1,
		.bufs = out_bufs,
		.bufferIdentifiers = out_buf_ids,
		.bufSizes = out_buf_sizes,
		.bufElSizes = out_buf_el_sizes,
	};
	AACENC_OutArgs out_args = {};

	res = aacEncEncode(this->aacenc, &in_buf_desc, &out_buf_desc, &in_args, &out_args);
	if (res != AACENC_OK)
		return -EINVAL;

	*dst_out = out_args.numOutBytes;
	*need_flush = NEED_FLUSH_ALL;

	/* RFC6416: It is set to 1 to indicate that the RTP packet contains a complete
   	 * audioMuxElement or the last fragment of an audioMuxElement */
	this->header->m = 1;

	return out_args.numInSamples * this->samplesize;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	const struct rtp_header *header = src;
	size_t header_size = sizeof(struct rtp_header);

	spa_return_val_if_fail (src_size > header_size, -EINVAL);

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	return header_size;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	uint data_size = (uint)src_size;
	uint bytes_valid = data_size;
	CStreamInfo *aacinf;
	int res;

	res = aacDecoder_Fill(this->aacdec, (UCHAR **)&src, &data_size, &bytes_valid);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "AAC buffer fill error: 0x%04X", res);
		return -EINVAL;
	}

	res = aacDecoder_DecodeFrame(this->aacdec, dst, dst_size, 0);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "AAC decode frame error: 0x%04X", res);
		return -EINVAL;
	}

	aacinf = aacDecoder_GetStreamInfo(this->aacdec);
	if (!aacinf) {
		spa_log_debug(log, "AAC get stream info failed");
		return -EINVAL;
	}
	*dst_out = aacinf->frameSize * aacinf->numChannels * this->samplesize;

	return src_size - bytes_valid;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_change_bitrate(struct impl *this, int new_bitrate)
{
	int res;

	new_bitrate = SPA_MIN(new_bitrate, this->max_bitrate);
	new_bitrate = SPA_MAX(new_bitrate, 64000);

	if (new_bitrate == this->cur_bitrate)
		return 0;

	this->cur_bitrate = new_bitrate;

	res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATE, this->cur_bitrate);
	if (res != AACENC_OK)
		return -EINVAL;

	return this->cur_bitrate;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;
	return codec_change_bitrate(this, (this->cur_bitrate * 2) / 3);
}

static int codec_increase_bitpool(void *data)
{
	struct impl *this = data;
	return codec_change_bitrate(this, (this->cur_bitrate * 4) / 3);
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &log_topic);
}

const struct media_codec a2dp_codec_aac = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_AAC,
	.codec_id = A2DP_CODEC_MPEG24,
	.name = "aac",
	.description = "AAC",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.init_props = codec_init_props,
	.clear_props = codec_clear_props,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.abr_process = codec_abr_process,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"aac",
	&a2dp_codec_aac
);
