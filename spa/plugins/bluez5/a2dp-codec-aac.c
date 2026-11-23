/* Spa A2DP AAC codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

#include <spa/param/audio/format.h>
#include <spa/utils/dict.h>

#if defined(HAVE_FDK_AAC)
#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>
#elif defined(HAVE_FAAC)
#include <faac.h>
#if defined(HAVE_FAAD3)
#include <faad3.h>
#else
#include <neaacdec.h>
#endif
#include "aac-latm.h"
#else
#error No AAC library
#endif

#include "rtp.h"
#include "media-codecs.h"

static struct spa_log *log;

#define DEFAULT_AAC_BITRATE	320000
#define MIN_AAC_BITRATE		64000

struct props {
	int bitratemode;
};

struct impl {
#if defined(HAVE_FDK_AAC)
	HANDLE_AACENCODER aacenc;
	HANDLE_AACDECODER aacdec;
#elif defined(HAVE_FAAC)
	faac_encoder *aacenc;
	uint8_t *outbuf;
	uint32_t outbuf_size;
	uint32_t frame_samples;
	uint32_t n_dropped;
	uint32_t n_bad_format;
	struct latm_enc mux;
#if defined(HAVE_FAAD3)
	faad_decoder *aacdec;
	uint32_t dec_max_output;
#else
	NeAACDecHandle aacdec;
#endif
	struct latm_dec demux;
	uint8_t dec_asc[2];
#endif

	struct rtp_header *header;

	size_t mtu;
	int codesize;

	int max_bitrate;
	int cur_bitrate;

	uint32_t rate;
	uint32_t channels;
	int samplesize;

	uint32_t enc_delay;
	uint32_t dec_delay;
};

static bool eld_supported(void)
{
	static bool supported = false, checked = false;

	if (checked)
		return supported;

#if defined(HAVE_FDK_AAC)
	HANDLE_AACENCODER aacenc = NULL;

	if (aacEncOpen(&aacenc, 0, 2) != AACENC_OK)
		goto done;
	if (aacEncoder_SetParam(aacenc, AACENC_AOT, AOT_ER_AAC_ELD) != AACENC_OK)
		goto done;
	if (aacEncoder_SetParam(aacenc,  AACENC_SBR_MODE, 1) != AACENC_OK)
		goto done;

	supported = true;

done:
	if (aacenc)
		aacEncClose(&aacenc);
#endif
	checked = true;
	spa_log_debug(log, "AAC-ELD support:%d", (int)supported);
	return supported;
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	uint8_t object_type;

	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_AAC_ELD) {
		if (!eld_supported())
			return -ENOTSUP;
		object_type = AAC_OBJECT_TYPE_MPEG4_AAC_ELD;
	} else {
		/* NOTE: AAC Long Term Prediction and AAC Scalable are
		 *       not supported by the FDK-AAC library. */
		object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC |
			AAC_OBJECT_TYPE_MPEG4_AAC_LC;
	}

	const a2dp_aac_t a2dp_aac = {
		.object_type = object_type,
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

static int codec_combine_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE], int caps_size,
		const uint8_t *other, int other_size)
{
	a2dp_aac_t *dst = (a2dp_aac_t *)caps;
	const a2dp_aac_t *src = (const a2dp_aac_t *)other;
	uint32_t freq;
	uint32_t dst_bitrate, src_bitrate;

	if (caps_size != (int)sizeof(*dst) || other_size != (int)sizeof(*src))
		return -EINVAL;

	dst->object_type |= src->object_type;
	dst->channels |= src->channels;
	dst->vbr |= src->vbr;

	freq = AAC_GET_FREQUENCY(*dst) | AAC_GET_FREQUENCY(*src);
	AAC_SET_FREQUENCY(*dst, freq);

	dst_bitrate = AAC_GET_BITRATE(*dst);
	src_bitrate = AAC_GET_BITRATE(*src);
	AAC_SET_BITRATE(*dst, SPA_MAX(dst_bitrate, src_bitrate));

	return caps_size;
}

static const struct media_codec_config
aac_frequencies[] = {
	{ AAC_SAMPLING_FREQ_44100, 44100, 10 },
	{ AAC_SAMPLING_FREQ_48000, 48000, 11 },
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
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_aac_t conf;
	int i;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	conf = *(a2dp_aac_t*)caps;

	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_AAC_ELD) {
		if (!eld_supported())
			return -ENOTSUP;

		if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_ELD)
			conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_ELD;
		else
			return -ENOTSUP;
	} else {
		/* PNS is only allowed with MPEG-4 AAC */
		uint8_t first = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
		uint8_t second = AAC_OBJECT_TYPE_MPEG2_AAC_LC;

		if (conf.object_type & first)
			conf.object_type = first;
		else if (conf.object_type & second)
			conf.object_type = second;
		else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
			return -ENOTSUP;  /* Not supported by FDK-AAC */
		else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
			return -ENOTSUP;  /* Not supported by FDK-AAC */
		else
			return -ENOTSUP;
	}

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
	 * bits for AAC object type. In addition AirPods set multiple bits.
	 *
	 * Some devices also set multiple bits in frequencies & channels.
	 * For these, pick a "preferred" choice.
	 *
	 * Each codec object only accepts its own object types: a2dp_codec_aac
	 * accepts LC variants, a2dp_codec_aac_eld accepts ELD. This lets the
	 * monitor's config-aware resolver attribute the transport to the
	 * correct codec object.
	 */
	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_AAC_ELD) {
		if (!(conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_ELD))
			return -EINVAL;
	} else {
		if (!(conf.object_type & (AAC_OBJECT_TYPE_MPEG2_AAC_LC |
						AAC_OBJECT_TYPE_MPEG4_AAC_LC)))
			return -EINVAL;
	}
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

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_audio_info info;
	struct spa_pod_frame f[1];
	int res;

	if ((res = codec_validate_config(codec, flags, caps, caps_size, &info)) < 0)
		return res;

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(info.media_type),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(info.media_subtype),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(info.info.raw.format),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(info.info.raw.rate),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(info.info.raw.channels),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, info.info.raw.channels, info.info.raw.position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static void *codec_init_props(const struct media_codec *codec, uint32_t flags, const struct spa_dict *settings)
{
	struct props *p = calloc(1, sizeof(struct props));
	const char *str;

	if (p == NULL)
		return NULL;

	if (settings == NULL || (str = spa_dict_lookup(settings, "bluez5.a2dp.aac.bitratemode")) == NULL)
		str = "5";

	p->bitratemode = SPA_CLAMP(atoi(str), 0, 5);
	return p;
}

static void codec_clear_props(void *props)
{
	free(props);
}

#if defined(HAVE_FDK_AAC)

/* Initialize this->aacenc/aacdec/codesize/max_bitrate/cur_bitrate. \return 0 on success, <0 on error */
static int init_aac(struct impl *this, const struct media_codec *codec,
		const a2dp_aac_t *conf, struct props *p)
{
	UINT bitratemode;
	int res;

	bitratemode = p ? p->bitratemode : 0;

	res = aacEncOpen(&this->aacenc, 0, this->channels);
	if (res != AACENC_OK)
		return -EINVAL;

	/* If object type has multiple bits set (invalid per spec, see above),
	 * the validate_config step is gated by codec->id so we trust the
	 * configuration matches this codec object. Within the LC family,
	 * prefer MPEG2 AAC LC which is mandatory.
	 */
	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_AAC_ELD) {
		if (!(conf->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_ELD))
			return -EINVAL;
		res = aacEncoder_SetParam(this->aacenc, AACENC_AOT, AOT_ER_AAC_ELD);
		if (res != AACENC_OK)
			return -EINVAL;

		res = aacEncoder_SetParam(this->aacenc,  AACENC_SBR_MODE, 1);
		if (res != AACENC_OK)
			return -EINVAL;
	} else if (conf->object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC) {
		res = aacEncoder_SetParam(this->aacenc, AACENC_AOT, AOT_MP2_AAC_LC);
		if (res != AACENC_OK)
			return -EINVAL;
	} else if (conf->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC) {
		res = aacEncoder_SetParam(this->aacenc, AACENC_AOT, AOT_AAC_LC);
		if (res != AACENC_OK)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	res = aacEncoder_SetParam(this->aacenc, AACENC_SAMPLERATE, this->rate);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncoder_SetParam(this->aacenc, AACENC_CHANNELMODE, this->channels);
	if (res != AACENC_OK)
		return -EINVAL;

	if (conf->vbr) {
		res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATEMODE,
				bitratemode);
		if (res != AACENC_OK)
			return -EINVAL;
	}

	res = aacEncoder_SetParam(this->aacenc, AACENC_AUDIOMUXVER, 2);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncoder_SetParam(this->aacenc, AACENC_SIGNALING_MODE, 1);
	if (res != AACENC_OK)
		return -EINVAL;

	// Fragmentation is not implemented yet,
	// so make sure every encoded AAC frame fits in (mtu - header)
	this->max_bitrate = ((this->mtu - sizeof(struct rtp_header)) * 8 * this->rate) / 1024;
	this->cur_bitrate = SPA_MIN(this->max_bitrate, get_valid_aac_bitrate((a2dp_aac_t *)conf));
	spa_log_debug(log, "AAC: max (peak) bitrate: %d, cur bitrate: %d, mode: %d (vbr: %d)",
			this->max_bitrate,
			this->cur_bitrate,
			bitratemode,
			conf->vbr);

	res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATE, this->cur_bitrate);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncoder_SetParam(this->aacenc, AACENC_PEAK_BITRATE, this->max_bitrate);
	if (res != AACENC_OK)
		return -EINVAL;

	// Assume >110 kbit/s as a "high bitrate" CBR and increase the
	// band pass cutout up to 19.3 kHz (as in mode 5 VBR).
	if (!conf->vbr && this->cur_bitrate > 110000)  {
		res = aacEncoder_SetParam(this->aacenc, AACENC_BANDWIDTH,
				19293);
		if (res != AACENC_OK)
			return -EINVAL;
	}

	res = aacEncoder_SetParam(this->aacenc, AACENC_TRANSMUX, TT_MP4_LATM_MCP1);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncoder_SetParam(this->aacenc, AACENC_HEADER_PERIOD, 1);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncoder_SetParam(this->aacenc, AACENC_AFTERBURNER, 1);
	if (res != AACENC_OK)
		return -EINVAL;

	res = aacEncEncode(this->aacenc, NULL, NULL, NULL, NULL);
	if (res != AACENC_OK)
		return -EINVAL;

	AACENC_InfoStruct enc_info = {};
	res = aacEncInfo(this->aacenc, &enc_info);
	if (res != AACENC_OK)
		return -EINVAL;

	this->enc_delay = enc_info.nDelay;

	this->codesize = enc_info.frameLength * this->channels * this->samplesize;

	this->aacdec = aacDecoder_Open(TT_MP4_LATM_MCP1, 1);
	if (!this->aacdec)
		return -EINVAL;

#ifdef AACDECODER_LIB_VL0
	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_MIN_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set min output channels: 0x%04X", res);
		return -EINVAL;
	}

	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_MAX_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set max output channels: 0x%04X", res);
		return -EINVAL;
	}
#else
	res = aacDecoder_SetParam(this->aacdec, AAC_PCM_OUTPUT_CHANNELS, this->channels);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "Couldn't set output channels: 0x%04X", res);
		return -EINVAL;
	}
#endif

	this->dec_delay = 0;

	return 0;
}

static void clear_aac(struct impl *this)
{
	if (this->aacenc)
		aacEncClose(&this->aacenc);
	if (this->aacdec)
		aacDecoder_Close(this->aacdec);
}

/* \return number of bytes consumed, or <0 on error */
static int encode_aac(struct impl *this, const void *src, size_t src_size,
		void *dst, size_t dst_size, size_t *dst_out, int *need_flush)
{
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

	return out_args.numInSamples * this->samplesize;
}

/* \return number of bytes consumed, or <0 on error */
static int decode_aac(struct impl *this, const void *src, size_t src_size,
		void *dst, size_t dst_size, size_t *dst_out)
{
	uint data_size = (uint)src_size;
	uint bytes_valid = data_size;
	CStreamInfo *aacinf;
	int res;

	res = aacDecoder_Fill(this->aacdec, (UCHAR **)&src, &data_size, &bytes_valid);
	if (res != AAC_DEC_OK) {
		spa_log_debug(log, "AAC buffer fill error: 0x%04X", res);
		return -EINVAL;
	}

	res = aacDecoder_DecodeFrame(this->aacdec, dst, dst_size / sizeof(INT_PCM), 0);
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

static int change_bitrate_aac(struct impl *this, int bitrate)
{
	int res = aacEncoder_SetParam(this->aacenc, AACENC_BITRATE, bitrate);
	if (res != AACENC_OK)
		return -EINVAL;
	return 0;
}

static void get_delay_aac(struct impl *this, uint32_t *encoder, uint32_t *decoder)
{
	if (encoder)
		*encoder = this->enc_delay;

	if (decoder) {
		CStreamInfo *info = aacDecoder_GetStreamInfo(this->aacdec);
		if (info)
			this->dec_delay = info->outputDelay;
		*decoder = this->dec_delay;
	}
}

#elif defined(HAVE_FAAC)

/*
 * Share of the peak bitrate used as the CBR target. At the peak itself the
 * rate control and the limiter fight and quality collapses on small MTUs.
 */
#ifndef FAAC_PEAK_MARGIN_PCT
#define FAAC_PEAK_MARGIN_PCT		95
#endif

static void close_decoder(struct impl *this)
{
	if (this->aacdec == NULL)
		return;
#if defined(HAVE_FAAD3)
	faad_decoder_close(&this->aacdec);
#else
	NeAACDecClose(this->aacdec);
#endif
	this->aacdec = NULL;
}

/* (Re)open the decoder for the given AudioSpecificConfig */
static int open_decoder(struct impl *this, const uint8_t asc[2])
{
	close_decoder(this);

#if defined(HAVE_FAAD3)
	faad_config cfg;
	faad_stream_info info = { .struct_size = sizeof(info) };

	if (faad_config_init(&cfg, sizeof(cfg)) < 0)
		return -EINVAL;
	cfg.stream_format = FAAD_STREAM_RAW;
	cfg.output_format = FAAD_OUTPUT_16BIT;
	cfg.downmix_mode = FAAD_DOWNMIX_NONE;

	if (faad_decoder_open(&cfg, asc, 2, &this->aacdec) < 0) {
		this->aacdec = NULL;
		return -EINVAL;
	}
	if (faad_decoder_get_info(this->aacdec, &info) < 0) {
		close_decoder(this);
		return -EINVAL;
	}
	this->dec_max_output = info.max_output_bytes;
#else
	NeAACDecConfigurationPtr cfg;
	unsigned long rate;
	unsigned char channels;
	signed char res;

	this->aacdec = NeAACDecOpen();
	if (this->aacdec == NULL)
		return -ENOMEM;

	cfg = NeAACDecGetCurrentConfiguration(this->aacdec);
	if (cfg == NULL) {
		close_decoder(this);
		return -EINVAL;
	}
	cfg->outputFormat = FAAD_FMT_16BIT;
	/* the stream is AAC-LC: don't assume implicit SBR at low sampling rates */
	cfg->dontUpSampleImplicitSBR = 1;
	if (!NeAACDecSetConfiguration(this->aacdec, cfg)) {
		close_decoder(this);
		return -EINVAL;
	}
	/* plain char, unsigned on some architectures */
	res = (signed char)NeAACDecInit2(this->aacdec, (unsigned char *)asc, 2, &rate, &channels);
	if (res < 0) {
		close_decoder(this);
		return -EINVAL;
	}
	/* Mono is reported as stereo with PS support, and the rate doubled up to
	 * 24 kHz when assuming implicit SBR. The decoded frames are checked later. */
	if ((rate != this->rate && !(this->rate <= 24000 && rate == 2 * this->rate)) ||
	    (channels != this->channels && !(this->channels == 1 && channels == 2))) {
		spa_log_warn(log, "AAC: decoder format %lu Hz %u ch differs from the negotiated one",
				rate, channels);
		close_decoder(this);
		return -EINVAL;
	}
#endif
	memcpy(this->dec_asc, asc, sizeof(this->dec_asc));
	return 0;
}

static int init_aac(struct impl *this, const struct media_codec *codec,
		const a2dp_aac_t *conf, struct props *p)
{
	faac_params fp;
	faac_encoder_info info = { .struct_size = sizeof(info) };
	const uint8_t *asc = NULL;
	uint32_t asc_len = 0;
	uint8_t asc0[2];
	size_t budget, overhead;
	bool mpeg4;

	/* If the object type has multiple bits set (invalid per spec), prefer
	 * MPEG-2 AAC LC, which is mandatory and does not allow PNS. */
	if (conf->object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
		mpeg4 = false;
	else if (conf->object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
		mpeg4 = true;
	else
		return -EINVAL;

	if (latm_make_asc(this->rate, this->channels, asc0) < 0 ||
	    latm_enc_init(&this->mux, asc0, sizeof(asc0)) < 0)
		return -EINVAL;

	/* Fragmentation is not supported by headsets, so every LATM packet
	 * must fit in (mtu - rtp header). */
	budget = this->mtu - sizeof(struct rtp_header);
	overhead = latm_enc_maxsize(&this->mux);
	if (budget <= overhead)
		return -EINVAL;

	this->max_bitrate = (int)SPA_MIN(((uint64_t)(budget - overhead) * 8 * this->rate) / 1024,
			/* ISO/IEC 14496-3: at most 6144 bits per channel in a frame */
			(uint64_t)this->channels * 6144 * this->rate / 1024);
	this->cur_bitrate = SPA_MIN(get_valid_aac_bitrate((a2dp_aac_t *)conf),
			this->max_bitrate / 100 * FAAC_PEAK_MARGIN_PCT);
	spa_log_debug(log, "AAC: max (peak) bitrate: %d, cur bitrate: %d, mpeg4: %d",
			this->max_bitrate, this->cur_bitrate, (int)mpeg4);

	if (faac_params_init(&fp, sizeof(fp)) < 0)
		return -EINVAL;
	fp.channels = this->channels;
	fp.sample_rate = this->rate;
	fp.object_type = FAAC_OBJ_LC;
	fp.input_format = FAAC_INPUT_16BIT;

	/* bit rates are for the whole stream */
	fp.stream_format = FAAC_STREAM_RAW;
	fp.rate_control = FAAC_RC_CBR;
	fp.bit_rate = this->cur_bitrate;
	fp.peak_bit_rate = this->max_bitrate;
	if (mpeg4)
		fp.flags |= FAAC_FLAG_PNS;
	else
		fp.flags &= ~(uint32_t)FAAC_FLAG_PNS;

	if (faac_encoder_open(&fp, &this->aacenc) < 0) {
		this->aacenc = NULL;
		return -EINVAL;
	}
	if (faac_encoder_get_info(this->aacenc, &info) < 0 ||
	    faac_encoder_asc(this->aacenc, &asc, &asc_len) < 0)
		return -EINVAL;

	if (info.object_type != fp.object_type || asc_len != sizeof(asc0) ||
	    memcmp(asc, asc0, sizeof(asc0)) != 0) {
		spa_log_warn(log, "AAC: unexpected encoder configuration");
		return -EINVAL;
	}

	this->frame_samples = info.frame_samples;
	this->codesize = info.frame_samples * this->channels * this->samplesize;
	this->enc_delay = info.encoder_delay;

	this->outbuf_size = info.max_output_bytes;
	this->outbuf = malloc(this->outbuf_size);
	if (this->outbuf == NULL)
		return -errno;

	latm_dec_init(&this->demux);
	this->dec_delay = 0;

	return open_decoder(this, asc0);
}

static void clear_aac(struct impl *this)
{
	if (this->aacenc)
		faac_encoder_close(&this->aacenc);
	close_decoder(this);
	free(this->outbuf);
	this->outbuf = NULL;
}

/* \return number of bytes consumed, or <0 on error */
static int encode_aac(struct impl *this, const void *src, size_t src_size,
		void *dst, size_t dst_size, size_t *dst_out, int *need_flush)
{
	uint32_t written = 0;
	int res;

	*dst_out = 0;
	*need_flush = NEED_FLUSH_NO;

	/* FAAC takes at most one frame per call; the caller loops over the rest */
	src_size = SPA_MIN(src_size, (size_t)this->codesize);

	if (faac_encoder_encode(this->aacenc, src, src_size / this->samplesize,
				this->outbuf, this->outbuf_size, &written) < 0)
		return -EINVAL;

	if (written > 0) {
		res = latm_enc_write(&this->mux, this->outbuf, written, dst, dst_size);
		if (res < 0 || (size_t)res > this->mtu - sizeof(struct rtp_header)) {
			/* A frame that doesn't fit the MTU can't be sent */
			this->n_dropped++;
			if ((this->n_dropped & (this->n_dropped - 1)) == 0)
				spa_log_warn(log, "AAC: dropped %u frame(s) not fitting the MTU (%u bytes)",
						this->n_dropped, written);
		} else {
			*dst_out = res;
			*need_flush = NEED_FLUSH_ALL;
		}
	}

	if (*dst_out == 0) {
		/* The block was consumed without producing a packet (encoder
		 * priming or dropped frame): keep the RTP timestamp in step. */
		this->header->timestamp = htonl(ntohl(this->header->timestamp) + this->frame_samples);
	}

	return src_size;
}

/* \return number of bytes consumed, or <0 on error */
static int decode_aac(struct impl *this, const void *src, size_t src_size,
		void *dst, size_t dst_size, size_t *dst_out)
{
	uint8_t frame[LATM_FRAME_MAXSIZE];
	bool changed;
	int res;

	*dst_out = 0;

	res = latm_dec_read(&this->demux, src, src_size, frame, sizeof(frame), &changed);
	if (res < 0) {
		spa_log_debug(log, "AAC LATM demux error: %d", res);
		return -EINVAL;
	}
	/* The port format is fixed: checked for every packet, not only on a config change */
	if ((uint32_t)latm_rates[this->demux.rate_index] != this->rate ||
	    (uint32_t)this->demux.channel_config != this->channels) {
		this->n_bad_format++;
		if ((this->n_bad_format & (this->n_bad_format - 1)) == 0)
			spa_log_warn(log, "AAC: dropped %u packet(s) with stream format %u Hz %d ch, expected %u Hz %u ch",
					this->n_bad_format, latm_rates[this->demux.rate_index],
					this->demux.channel_config, this->rate, this->channels);
		return -EINVAL;
	}
	if (changed && memcmp(this->dec_asc, this->demux.asc, sizeof(this->dec_asc)) != 0 &&
	    open_decoder(this, this->demux.asc) < 0)
		return -EINVAL;
	if (res == 0)
		return src_size;

#if defined(HAVE_FAAD3)
	faad_stream_info info = { .struct_size = sizeof(info) };
	uint32_t consumed = 0, written = 0, frame_flags = 0;

	if (dst_size < this->dec_max_output)
		return -ENOSPC;

	if (faad_decode_frame(this->aacdec, frame, res, &consumed,
				dst, dst_size, &written, &frame_flags) < 0) {
		spa_log_debug(log, "AAC decode frame error");
		return -EINVAL;
	}
	if (written > 0 && (frame_flags & FAAD_FRAME_FORMAT_CHANGED)) {
		if (faad_decoder_get_info(this->aacdec, &info) < 0 ||
		    info.sample_rate != this->rate || info.channels != this->channels) {
			spa_log_warn(log, "AAC: decoded format differs from the negotiated one");
			return -EINVAL;
		}
	}
	*dst_out = written;
#else
	NeAACDecFrameInfo info;
	void *pcm;
	size_t size;

	pcm = NeAACDecDecode(this->aacdec, &info, frame, res);
	if (info.error != 0 || pcm == NULL || info.channels == 0) {
		spa_log_debug(log, "AAC decode frame error: %u", (unsigned)info.error);
		return -EINVAL;
	}
	if (info.samplerate != this->rate ||
	    (info.channels != this->channels && !(this->channels == 1 && info.channels == 2))) {
		spa_log_warn(log, "AAC: decoded format differs from the negotiated one");
		return -EINVAL;
	}
	size = (size_t)info.samples / info.channels * this->channels * this->samplesize;
	if (size > dst_size)
		return -ENOSPC;
	if (info.channels == this->channels) {
		memcpy(dst, pcm, size);
	} else {
		/* mono reported as stereo: keep the first channel */
		const int16_t *s = pcm;
		int16_t *d = dst;
		size_t i;

		for (i = 0; i < size / this->samplesize; i++)
			d[i] = s[2 * i];
	}
	*dst_out = size;
#endif

	return src_size;
}

static int change_bitrate_aac(struct impl *this, int bitrate)
{
	return -EOPNOTSUPP;
}

static void get_delay_aac(struct impl *this, uint32_t *encoder, uint32_t *decoder)
{
	if (encoder)
		*encoder = this->enc_delay;
	if (decoder) {
#if defined(HAVE_FAAD3)
		faad_stream_info info = { .struct_size = sizeof(info) };

		if (this->aacdec && faad_decoder_get_info(this->aacdec, &info) >= 0)
			this->dec_delay = info.decoder_delay;
#endif
		/* FAAD2 reports none, and AAC-LC adds none */
		*decoder = this->dec_delay;
	}
}

#endif

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	a2dp_aac_t *conf = config;
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

	if ((res = init_aac(this, codec, conf, props)) < 0)
		goto error;

	return this;

error:
	if (this)
		clear_aac(this);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	clear_aac(this);
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

	res = encode_aac(this, src, src_size, dst, dst_size, dst_out, need_flush);
	if (res < 0)
		return res;

	/* RFC6416: It is set to 1 to indicate that the RTP packet contains a complete
	 * audioMuxElement or the last fragment of an audioMuxElement */
	if (*need_flush)
		this->header->m = 1;

	return res;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	const struct rtp_header *header = src;
	size_t header_size = sizeof(struct rtp_header);

	if (src_size <= header_size)
		return -EINVAL;

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

	return decode_aac(this, src, src_size, dst, dst_size, dst_out);
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

	res = change_bitrate_aac(this, new_bitrate);
	if (res < 0)
		return res;

	this->cur_bitrate = new_bitrate;

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

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	struct impl *this = data;

	get_delay_aac(this, encoder, decoder);
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_aac = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_AAC,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_MPEG24,
	.name = "aac",
	.description = "AAC",
	.fill_caps = codec_fill_caps,
	.combine_caps = codec_combine_caps,
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
	.get_delay = codec_get_delay,
};

#if defined(HAVE_FDK_AAC)
const struct media_codec a2dp_codec_aac_eld = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_AAC_ELD,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_MPEG24,
	.name = "aac_eld",
	.description = "AAC-ELD",
	.endpoint_name = "aac",
	.endpoint_companion = true,
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
	.get_delay = codec_get_delay,
};
#endif

MEDIA_CODEC_EXPORT_DEF(
	"aac",
#if defined(HAVE_FDK_AAC)
	&a2dp_codec_aac,
	&a2dp_codec_aac_eld
#else
	&a2dp_codec_aac
#endif
);
