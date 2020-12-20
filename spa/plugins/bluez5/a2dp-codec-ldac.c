/* Spa A2DP LDAC codec
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

#include <spa/param/audio/format.h>

#include <ldacBT.h>

#ifdef ENABLE_LDAC_ABR
#include <ldacBT_abr.h>
#endif

#include "defs.h"
#include "rtp.h"
#include "a2dp-codecs.h"

#define LDAC_ABR_MAX_PACKET_NBYTES 1280

#define LDAC_ABR_INTERVAL_MS 5 /* 2 frames * 128 lsu / 48000 */

/* decrease ABR thresholds to increase stability */
#define LDAC_ABR_THRESHOLD_CRITICAL 6
#define LDAC_ABR_THRESHOLD_DANGEROUSTREND 4
#define LDAC_ABR_THRESHOLD_SAFETY_FOR_HQSQ 3


struct impl {
	HANDLE_LDAC_BT ldac;
#ifdef ENABLE_LDAC_ABR
	HANDLE_LDAC_ABR ldac_abr;
	bool enable_abr;
#endif
	struct rtp_header *header;
	struct rtp_payload *payload;

	int mtu;
	int eqmid;
	int frequency;
	int fmt;
	int codesize;
	int frame_length;
	int frame_count;
	int frame_count_factor;
};

enum {
	LDACBT_EQMID_BITRATE_990000 = 0, /* LDACBT_EQMID_HQ */
	LDACBT_EQMID_BITRATE_660000,     /* LDACBT_EQMID_SQ */
	LDACBT_EQMID_BITRATE_330000,	 /* LDACBT_EQMID_MQ */

	LDACBT_EQMID_BITRATE_492000,
	LDACBT_EQMID_BITRATE_396000,
	LDACBT_EQMID_BITRATE_282000,
	LDACBT_EQMID_BITRATE_246000,
	LDACBT_EQMID_BITRATE_216000,
	LDACBT_EQMID_BITRATE_198000,
	LDACBT_EQMID_BITRATE_180000,
	LDACBT_EQMID_BITRATE_162000,
	LDACBT_EQMID_BITRATE_150000,
	LDACBT_EQMID_BITRATE_138000
};

struct ldac_config
{
    int eqmid;
    int frame_count; 	   /* number of ldac frames in packet */
    int frame_length;      /* ldac frame length */
    int frame_length_1ch;  /* ldac frame length per channel */
};

static const struct ldac_config ldac_config_table[] = {
	{ LDACBT_EQMID_BITRATE_990000,     2,    330,   165},
	{ LDACBT_EQMID_BITRATE_660000,     3,    220,   110},
	{ LDACBT_EQMID_BITRATE_492000,     4,    164,    82},
	{ LDACBT_EQMID_BITRATE_396000,     5,    132,    66},
	{ LDACBT_EQMID_BITRATE_330000,     6,    110,    55},
	{ LDACBT_EQMID_BITRATE_282000,     7,     94,    47},
	{ LDACBT_EQMID_BITRATE_246000,     8,     82,    41},
	{ LDACBT_EQMID_BITRATE_216000,     9,     72,    36},
	{ LDACBT_EQMID_BITRATE_198000,    10,     66,    33},
	{ LDACBT_EQMID_BITRATE_180000,    11,     60,    30},
	{ LDACBT_EQMID_BITRATE_162000,    12,     54,    27},
	{ LDACBT_EQMID_BITRATE_150000,    13,     50,    25},
	{ LDACBT_EQMID_BITRATE_138000,    14,     46,    23},
};

static int codec_fill_caps(const struct a2dp_codec *codec, uint32_t flags, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_ldac_t a2dp_ldac = {
		.info.vendor_id = LDAC_VENDOR_ID,
		.info.codec_id = LDAC_CODEC_ID,
		.frequency = LDACBT_SAMPLING_FREQ_044100 |
			LDACBT_SAMPLING_FREQ_048000 |
			LDACBT_SAMPLING_FREQ_088200 |
			LDACBT_SAMPLING_FREQ_096000,
		.channel_mode = LDACBT_CHANNEL_MODE_MONO |
			LDACBT_CHANNEL_MODE_DUAL_CHANNEL |
			LDACBT_CHANNEL_MODE_STEREO,
	};
	memcpy(caps, &a2dp_ldac, sizeof(a2dp_ldac));
	return sizeof(a2dp_ldac);
}

static int codec_select_config(const struct a2dp_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct spa_audio_info *info, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_ldac_t conf;

        if (caps_size < sizeof(conf))
                return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));
	conf.info.vendor_id = LDAC_VENDOR_ID;
	conf.info.codec_id = LDAC_CODEC_ID;

	if (conf.frequency & LDACBT_SAMPLING_FREQ_044100)
		conf.frequency = LDACBT_SAMPLING_FREQ_044100;
	else if (conf.frequency & LDACBT_SAMPLING_FREQ_048000)
		conf.frequency = LDACBT_SAMPLING_FREQ_048000;
	else if (conf.frequency & LDACBT_SAMPLING_FREQ_088200)
		conf.frequency = LDACBT_SAMPLING_FREQ_088200;
	else if (conf.frequency & LDACBT_SAMPLING_FREQ_096000)
		conf.frequency = LDACBT_SAMPLING_FREQ_096000;
	else
		return -ENOTSUP;

	if (conf.channel_mode & LDACBT_CHANNEL_MODE_STEREO)
		conf.channel_mode = LDACBT_CHANNEL_MODE_STEREO;
        else if (conf.channel_mode & LDACBT_CHANNEL_MODE_DUAL_CHANNEL)
		conf.channel_mode = LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
        else if (conf.channel_mode & LDACBT_CHANNEL_MODE_MONO)
		conf.channel_mode = LDACBT_CHANNEL_MODE_MONO;
	else
		return -ENOTSUP;

	memcpy(config, &conf, sizeof(conf));

        return sizeof(conf);
}

static int codec_enum_config(const struct a2dp_codec *codec,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_ldac_t conf;
        struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t i = 0;
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(5,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_F32,
								SPA_AUDIO_FORMAT_S32,
								SPA_AUDIO_FORMAT_S24,
								SPA_AUDIO_FORMAT_S16),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if (conf.frequency & LDACBT_SAMPLING_FREQ_048000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.frequency & LDACBT_SAMPLING_FREQ_044100) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (conf.frequency & LDACBT_SAMPLING_FREQ_088200) {
		if (i++ == 0)
			spa_pod_builder_int(b, 88200);
		spa_pod_builder_int(b, 88200);
	}
	if (conf.frequency & LDACBT_SAMPLING_FREQ_096000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 96000);
		spa_pod_builder_int(b, 96000);
	}
	if (i == 0)
		return -EINVAL;
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (conf.channel_mode & LDACBT_CHANNEL_MODE_MONO &&
	    conf.channel_mode & (LDACBT_CHANNEL_MODE_STEREO |
		    LDACBT_CHANNEL_MODE_DUAL_CHANNEL)) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 2),
				0);
	} else if (conf.channel_mode & LDACBT_CHANNEL_MODE_MONO) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 1, position),
				0);
	} else {
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 2, position),
				0);
	}
	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int update_frame_info(struct impl *this)
{
	const struct ldac_config *config;
	this->eqmid = ldacBT_get_eqmid(this->ldac);
	for (size_t i = 0; i < SPA_N_ELEMENTS(ldac_config_table); ++i) {
		config = &ldac_config_table[i];

		if (config->eqmid != this->eqmid)
			continue;

		this->frame_count = config->frame_count;
		this->frame_length = config->frame_length;
		return 0;
	}
	return -EINVAL;
}

static int codec_reduce_bitpool(void *data)
{
#ifdef ENABLE_LDAC_ABR
	return -EINVAL;
#else
	struct impl *this = data;
	int res;
	if (this->eqmid == LDACBT_EQMID_BITRATE_330000)
		return this->eqmid;
	res = ldacBT_alter_eqmid_priority(this->ldac, LDACBT_EQMID_INC_CONNECTION);
	update_frame_info(this);
	return res;
#endif
}

static int codec_increase_bitpool(void *data)
{
#ifdef ENABLE_LDAC_ABR
	return -EINVAL;
#else
	struct impl *this = data;
	int res;
	res = ldacBT_alter_eqmid_priority(this->ldac, LDACBT_EQMID_INC_QUALITY);
	update_frame_info(this);
	return res;
#endif
}

static int codec_get_num_blocks(void *data)
{
	struct impl *this = data;
	return this->frame_count * this->frame_count_factor;
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static void *codec_init(const struct a2dp_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info, size_t mtu)
{
	struct impl *this;
	a2dp_ldac_t *conf = config;
	int res;

	this = calloc(1, sizeof(struct impl));
	if (this == NULL)
		goto error_errno;

	this->ldac = ldacBT_get_handle();
	if (this->ldac == NULL)
		goto error_errno;

#ifdef ENABLE_LDAC_ABR
	this->ldac_abr = ldac_ABR_get_handle();
	if (this->ldac_abr == NULL)
		goto error_errno;
#endif

	this->eqmid = LDACBT_EQMID_SQ;
	this->mtu = mtu;
	this->frequency = info->info.raw.rate;
	this->codesize = info->info.raw.channels;

	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_F32:
		this->fmt = LDACBT_SMPL_FMT_F32;
		this->codesize *= 4;
		break;
	case SPA_AUDIO_FORMAT_S32:
		this->fmt = LDACBT_SMPL_FMT_S32;
		this->codesize *= 4;
		break;
	case SPA_AUDIO_FORMAT_S24:
		this->fmt = LDACBT_SMPL_FMT_S24;
		this->codesize *= 3;
		break;
	case SPA_AUDIO_FORMAT_S16:
		this->fmt = LDACBT_SMPL_FMT_S16;
		this->codesize *= 2;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	switch(conf->frequency) {
	case LDACBT_SAMPLING_FREQ_044100:
	case LDACBT_SAMPLING_FREQ_048000:
		this->codesize *= 128;
		this->frame_count_factor = 1;
		break;
	case LDACBT_SAMPLING_FREQ_088200:
	case LDACBT_SAMPLING_FREQ_096000:
		/* ldac ecoder has a constant encoding lsu: LDACBT_ENC_LSU=128 */
		this->codesize *= 128;
		this->frame_count_factor = 2;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	res = ldacBT_init_handle_encode(this->ldac,
			this->mtu,
			this->eqmid,
			conf->channel_mode,
			this->fmt,
			this->frequency);
	if (res < 0)
		goto error;

#ifdef ENABLE_LDAC_ABR
	res = ldac_ABR_Init(this->ldac_abr, LDAC_ABR_INTERVAL_MS);
	if (res < 0)
		goto error;

	res = ldac_ABR_set_thresholds(this->ldac_abr,
		LDAC_ABR_THRESHOLD_CRITICAL,
		LDAC_ABR_THRESHOLD_DANGEROUSTREND,
		LDAC_ABR_THRESHOLD_SAFETY_FOR_HQSQ);
	if (res < 0)
		goto error;

	this->enable_abr = true;
#endif

	update_frame_info(this);

	return this;

error_errno:
	res = -errno;
error:
	if (this->ldac)
		ldacBT_free_handle(this->ldac);
#ifdef ENABLE_LDAC_ABR
	if (this->ldac_abr)
		ldac_ABR_free_handle(this->ldac_abr);
#endif
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	if (this->ldac)
		ldacBT_free_handle(this->ldac);
#ifdef ENABLE_LDAC_ABR
	if (this->ldac_abr)
		ldac_ABR_free_handle(this->ldac_abr);
#endif
	free(this);
}

static int codec_abr_process(void *data, size_t unsent)
{
#ifdef ENABLE_LDAC_ABR
	struct impl *this = data;
	int res;
	res = ldac_ABR_Proc(this->ldac, this->ldac_abr,
			unsent / LDAC_ABR_MAX_PACKET_NBYTES, this->enable_abr);
	update_frame_info(this);
	return res;
#else
	return -EINVAL;
#endif
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;

	this->header = (struct rtp_header *)dst;
	this->payload = SPA_MEMBER(dst, sizeof(struct rtp_header), struct rtp_payload);
	memset(this->header, 0, sizeof(struct rtp_header)+sizeof(struct rtp_payload));

	this->payload->frame_count = 0;
	this->header->v = 2;
	this->header->pt = 1;
	this->header->sequence_number = htons(seqnum);
	this->header->timestamp = htonl(timestamp);
	this->header->ssrc = htonl(1);
	return sizeof(struct rtp_header) + sizeof(struct rtp_payload);
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res, src_used, dst_used, frame_num = 0;

	src_used = src_size;
	dst_used = dst_size;

	res = ldacBT_encode(this->ldac, (void*)src, &src_used, dst, &dst_used, &frame_num);
	if (res < 0)
		return -EINVAL;

	*dst_out = dst_used;

	this->payload->frame_count += frame_num;

	return src_used;
}

const struct a2dp_codec a2dp_codec_ldac = {
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = LDAC_VENDOR_ID,
		.codec_id = LDAC_CODEC_ID },
	.name = "ldac",
	.description = "LDAC",
	.send_fill_frames = 4,
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.get_num_blocks = codec_get_num_blocks,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
};
