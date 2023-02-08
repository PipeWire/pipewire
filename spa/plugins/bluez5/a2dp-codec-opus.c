/* Spa A2DP Opus Codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>
#if __BYTE_ORDER != __LITTLE_ENDIAN
#include <byteswap.h>
#endif

#include <spa/debug/types.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/string.h>
#include <spa/utils/dict.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>

#include <opus.h>
#include <opus_multistream.h>

#include "rtp.h"
#include "media-codecs.h"

static struct spa_log *log;
static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.codecs.opus");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define BUFSIZE_FROM_BITRATE(frame_dms,bitrate)	((bitrate)/8 * (frame_dms) / 10000 * 5/4)  /* estimate */

/*
 * Opus CVBR target bitrate. When connecting, it is set to the INITIAL
 * value, and after that adjusted according to link quality between the MIN and
 * MAX values. The bitrate adjusts up to either MAX or the value at
 * which the socket buffer starts filling up, whichever is lower.
 *
 * With perfect connection quality, the target bitrate converges to the MAX
 * value. Under realistic conditions, the upper limit may often be as low as
 * 300-500kbit/s, so the INITIAL values are not higher than this.
 *
 * The MAX is here set to 2-2.5x and INITIAL to 1.5x the upper Opus recommended
 * values [1], to be safer quality-wise for CVBR, and MIN to the lower
 * recommended value.
 *
 * [1] https://wiki.xiph.org/Opus_Recommended_Settings
 */
#define BITRATE_INITIAL			192000
#define BITRATE_MAX			320000
#define BITRATE_MIN			96000

#define BITRATE_INITIAL_51		384000
#define BITRATE_MAX_51			600000
#define BITRATE_MIN_51			128000

#define BITRATE_INITIAL_71		450000
#define BITRATE_MAX_71			900000
#define BITRATE_MIN_71			256000

#define BITRATE_DUPLEX_BIDI		160000

#define OPUS_05_MAX_BYTES	(15 * 1024)

struct props {
	uint32_t channels;
	uint32_t coupled_streams;
	uint32_t location;
	uint32_t max_bitrate;
	uint8_t frame_duration;
	int application;

	uint32_t bidi_channels;
	uint32_t bidi_coupled_streams;
	uint32_t bidi_location;
	uint32_t bidi_max_bitrate;
	uint32_t bidi_frame_duration;
	int bidi_application;
};

struct dec_data {
	int fragment_size;
	int fragment_count;
	uint8_t fragment[OPUS_05_MAX_BYTES];
};

struct abr {
	uint64_t now;
	uint64_t last_update;

	uint32_t buffer_level;
	uint32_t packet_size;
	uint32_t total_size;
	bool bad;

	uint64_t last_change;
	uint64_t retry_interval;

	bool prev_bad;
};

struct enc_data {
	struct rtp_header *header;
	struct rtp_payload *payload;

	struct abr abr;

	int samples;
	int codesize;

	int packet_size;
	int fragment_size;
	int fragment_count;
	void *fragment;

	int bitrate_min;
	int bitrate_max;

	int bitrate;
	int next_bitrate;

	int frame_dms;
	int application;
};

struct impl {
	OpusMSEncoder *enc;
	OpusMSDecoder *dec;

	int mtu;
	int samplerate;
	int application;

	uint8_t channels;
	uint8_t streams;
	uint8_t coupled_streams;

	bool is_bidi;

	struct dec_data d;
	struct enc_data e;
};

struct audio_location {
	uint32_t mask;
	enum spa_audio_channel position;
};

struct surround_encoder_mapping {
	uint8_t channels;
	uint8_t coupled_streams;
	uint32_t location;
	uint8_t mapping[8];		/**< permutation streams -> vorbis order */
	uint8_t inv_mapping[8];		/**< permutation vorbis order -> streams */
};

/* Bluetooth SIG, Assigned Numbers, Generic Audio, Audio Location Definitions */
#define BT_AUDIO_LOCATION_FL	0x00000001  /* Front Left */
#define BT_AUDIO_LOCATION_FR	0x00000002  /* Front Right */
#define BT_AUDIO_LOCATION_FC	0x00000004  /* Front Center */
#define BT_AUDIO_LOCATION_LFE	0x00000008  /* Low Frequency Effects 1 */
#define BT_AUDIO_LOCATION_RL	0x00000010  /* Back Left */
#define BT_AUDIO_LOCATION_RR	0x00000020  /* Back Right */
#define BT_AUDIO_LOCATION_FLC	0x00000040  /* Front Left of Center */
#define BT_AUDIO_LOCATION_FRC	0x00000080  /* Front Right of Center */
#define BT_AUDIO_LOCATION_RC	0x00000100  /* Back Center */
#define BT_AUDIO_LOCATION_LFE2	0x00000200  /* Low Frequency Effects 2 */
#define BT_AUDIO_LOCATION_SL	0x00000400  /* Side Left */
#define BT_AUDIO_LOCATION_SR	0x00000800  /* Side Right */
#define BT_AUDIO_LOCATION_TFL	0x00001000  /* Top Front Left */
#define BT_AUDIO_LOCATION_TFR	0x00002000  /* Top Front Right */
#define BT_AUDIO_LOCATION_TFC	0x00004000  /* Top Front Center */
#define BT_AUDIO_LOCATION_TC	0x00008000  /* Top Center */
#define BT_AUDIO_LOCATION_TRL	0x00010000  /* Top Back Left */
#define BT_AUDIO_LOCATION_TRR	0x00020000  /* Top Back Right */
#define BT_AUDIO_LOCATION_TSL	0x00040000  /* Top Side Left */
#define BT_AUDIO_LOCATION_TSR	0x00080000  /* Top Side Right */
#define BT_AUDIO_LOCATION_TRC	0x00100000  /* Top Back Center */
#define BT_AUDIO_LOCATION_BC	0x00200000  /* Bottom Front Center */
#define BT_AUDIO_LOCATION_BLC	0x00400000  /* Bottom Front Left */
#define BT_AUDIO_LOCATION_BRC	0x00800000  /* Bottom Front Right */
#define BT_AUDIO_LOCATION_FLW	0x01000000  /* Fron Left Wide */
#define BT_AUDIO_LOCATION_FRW	0x02000000  /* Front Right Wide */
#define BT_AUDIO_LOCATION_SSL	0x04000000  /* Left Surround */
#define BT_AUDIO_LOCATION_SSR	0x08000000  /* Right Surround */

#define BT_AUDIO_LOCATION_ANY	0x0fffffff

static const struct audio_location audio_locations[] = {
	{ BT_AUDIO_LOCATION_FL, SPA_AUDIO_CHANNEL_FL },
	{ BT_AUDIO_LOCATION_FR, SPA_AUDIO_CHANNEL_FR },
	{ BT_AUDIO_LOCATION_SL, SPA_AUDIO_CHANNEL_SL },
	{ BT_AUDIO_LOCATION_SR, SPA_AUDIO_CHANNEL_SR },
	{ BT_AUDIO_LOCATION_RL, SPA_AUDIO_CHANNEL_RL },
	{ BT_AUDIO_LOCATION_RR, SPA_AUDIO_CHANNEL_RR },
	{ BT_AUDIO_LOCATION_FLC, SPA_AUDIO_CHANNEL_FLC },
	{ BT_AUDIO_LOCATION_FRC, SPA_AUDIO_CHANNEL_FRC },
	{ BT_AUDIO_LOCATION_TFL, SPA_AUDIO_CHANNEL_TFL },
	{ BT_AUDIO_LOCATION_TFR, SPA_AUDIO_CHANNEL_TFR },
	{ BT_AUDIO_LOCATION_TSL, SPA_AUDIO_CHANNEL_TSL },
	{ BT_AUDIO_LOCATION_TSR, SPA_AUDIO_CHANNEL_TSR },
	{ BT_AUDIO_LOCATION_TRL, SPA_AUDIO_CHANNEL_TRL },
	{ BT_AUDIO_LOCATION_TRR, SPA_AUDIO_CHANNEL_TRR },
	{ BT_AUDIO_LOCATION_BLC, SPA_AUDIO_CHANNEL_BLC },
	{ BT_AUDIO_LOCATION_BRC, SPA_AUDIO_CHANNEL_BRC },
	{ BT_AUDIO_LOCATION_FLW, SPA_AUDIO_CHANNEL_FLW },
	{ BT_AUDIO_LOCATION_FRW, SPA_AUDIO_CHANNEL_FRW },
	{ BT_AUDIO_LOCATION_SSL, SPA_AUDIO_CHANNEL_SL },  /* ~ Side Left */
	{ BT_AUDIO_LOCATION_SSR, SPA_AUDIO_CHANNEL_SR },  /* ~ Side Right */
	{ BT_AUDIO_LOCATION_FC, SPA_AUDIO_CHANNEL_FC },
	{ BT_AUDIO_LOCATION_RC, SPA_AUDIO_CHANNEL_RC },
	{ BT_AUDIO_LOCATION_TFC, SPA_AUDIO_CHANNEL_TFC },
	{ BT_AUDIO_LOCATION_TC, SPA_AUDIO_CHANNEL_TC },
	{ BT_AUDIO_LOCATION_TRC, SPA_AUDIO_CHANNEL_TRC },
	{ BT_AUDIO_LOCATION_BC, SPA_AUDIO_CHANNEL_BC },
	{ BT_AUDIO_LOCATION_LFE, SPA_AUDIO_CHANNEL_LFE },
	{ BT_AUDIO_LOCATION_LFE2, SPA_AUDIO_CHANNEL_LFE2 },
};

/* Opus surround encoder mapping tables for the supported channel configurations */
static const struct surround_encoder_mapping surround_encoders[] = {
	{ 1, 0, (0x0),
	  { 0 }, { 0 } },
	{ 2, 1, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR),
	  { 0, 1 }, { 0, 1 } },
	{ 3, 1, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_FC),
	  { 0, 2, 1 }, { 0, 2, 1 } },
	{ 4, 2, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_RL |
				BT_AUDIO_LOCATION_RR),
	  { 0, 1, 2, 3 }, { 0, 1, 2, 3 } },
	{ 5, 2, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_RL |
				BT_AUDIO_LOCATION_RR | BT_AUDIO_LOCATION_FC),
	  { 0, 4, 1, 2, 3 }, { 0, 2, 3, 4, 1 } },
	{ 6, 2, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_RL |
				BT_AUDIO_LOCATION_RR | BT_AUDIO_LOCATION_FC |
				BT_AUDIO_LOCATION_LFE),
	  { 0, 4, 1, 2, 3, 5 }, { 0, 2, 3, 4, 1, 5 } },
	{ 7, 3, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_SL |
				BT_AUDIO_LOCATION_SR | BT_AUDIO_LOCATION_FC |
				BT_AUDIO_LOCATION_RC | BT_AUDIO_LOCATION_LFE),
	  { 0, 4, 1, 2, 3, 5, 6 }, { 0, 2, 3, 4, 1, 5, 6 } },
	{ 8, 3, (BT_AUDIO_LOCATION_FL | BT_AUDIO_LOCATION_FR | BT_AUDIO_LOCATION_SL |
				BT_AUDIO_LOCATION_SR | BT_AUDIO_LOCATION_RL |
				BT_AUDIO_LOCATION_RR | BT_AUDIO_LOCATION_FC |
				BT_AUDIO_LOCATION_LFE),
	  { 0, 6, 1, 2, 3, 4, 5, 7 }, { 0, 2, 3, 4, 5, 6, 1, 7 } },
};

static uint32_t bt_channel_from_name(const char *name)
{
	size_t i;
	enum spa_audio_channel position = SPA_AUDIO_CHANNEL_UNKNOWN;

	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name))) {
			position = spa_type_audio_channel[i].type;
			break;
		}
	}
	for (i = 0; i < SPA_N_ELEMENTS(audio_locations); i++) {
		if (position == audio_locations[i].position)
			return audio_locations[i].mask;
	}
	return 0;
}

static uint32_t parse_locations(const char *str)
{
	char *s, *p, *save = NULL;
	uint32_t location = 0;

	if (!str)
		return 0;

	s = strdup(str);
	if (s == NULL)
		return 0;

	for (p = s; (p = strtok_r(p, ", ", &save)) != NULL; p = NULL) {
		if (*p == '\0')
			continue;
		location |= bt_channel_from_name(p);
	}
	free(s);

	return location;
}

static void parse_settings(struct props *props, const struct spa_dict *settings)
{
	const char *str;
	uint32_t v;

	/* Pro Audio settings */
	spa_zero(*props);
	props->channels = 8;
	props->coupled_streams = 0;
	props->location = 0;
	props->max_bitrate = BITRATE_MAX;
	props->frame_duration = OPUS_05_FRAME_DURATION_100;
	props->application = OPUS_APPLICATION_AUDIO;

	props->bidi_channels = 1;
	props->bidi_coupled_streams = 0;
	props->bidi_location = 0;
	props->bidi_max_bitrate = BITRATE_DUPLEX_BIDI;
	props->bidi_frame_duration = OPUS_05_FRAME_DURATION_400;
	props->bidi_application = OPUS_APPLICATION_AUDIO;

	if (settings == NULL)
		return;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.channels"), &v, 0))
		props->channels = SPA_CLAMP(v, 1u, SPA_AUDIO_MAX_CHANNELS);
	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.max-bitrate"), &v, 0))
		props->max_bitrate = SPA_MAX(v, (uint32_t)BITRATE_MIN);
	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.coupled-streams"), &v, 0))
		props->coupled_streams = SPA_CLAMP(v, 0u, props->channels / 2);

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.channels"), &v, 0))
		props->bidi_channels = SPA_CLAMP(v, 0u, SPA_AUDIO_MAX_CHANNELS);
	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.max-bitrate"), &v, 0))
		props->bidi_max_bitrate = SPA_MAX(v, (uint32_t)BITRATE_MIN);
	if (spa_atou32(spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.coupled-streams"), &v, 0))
		props->bidi_coupled_streams = SPA_CLAMP(v, 0u, props->bidi_channels / 2);

	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.locations");
	props->location = parse_locations(str);

	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.locations");
	props->bidi_location = parse_locations(str);

	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.frame-dms");
	if (spa_streq(str, "25"))
		props->frame_duration = OPUS_05_FRAME_DURATION_25;
	else if (spa_streq(str, "50"))
		props->frame_duration = OPUS_05_FRAME_DURATION_50;
	else if (spa_streq(str, "100"))
		props->frame_duration = OPUS_05_FRAME_DURATION_100;
	else if (spa_streq(str, "200"))
		props->frame_duration = OPUS_05_FRAME_DURATION_200;
	else if (spa_streq(str, "400"))
		props->frame_duration = OPUS_05_FRAME_DURATION_400;

	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.frame-dms");
	if (spa_streq(str, "25"))
		props->bidi_frame_duration = OPUS_05_FRAME_DURATION_25;
	else if (spa_streq(str, "50"))
		props->bidi_frame_duration = OPUS_05_FRAME_DURATION_50;
	else if (spa_streq(str, "100"))
		props->bidi_frame_duration = OPUS_05_FRAME_DURATION_100;
	else if (spa_streq(str, "200"))
		props->bidi_frame_duration = OPUS_05_FRAME_DURATION_200;
	else if (spa_streq(str, "400"))
		props->bidi_frame_duration = OPUS_05_FRAME_DURATION_400;

	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.application");
	if (spa_streq(str, "audio"))
		props->application = OPUS_APPLICATION_AUDIO;
	else if (spa_streq(str, "voip"))
		props->application = OPUS_APPLICATION_VOIP;
	else if (spa_streq(str, "lowdelay"))
		props->application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;


	str = spa_dict_lookup(settings, "bluez5.a2dp.opus.pro.bidi.application");
	if (spa_streq(str, "audio"))
		props->bidi_application = OPUS_APPLICATION_AUDIO;
	else if (spa_streq(str, "voip"))
		props->bidi_application = OPUS_APPLICATION_VOIP;
	else if (spa_streq(str, "lowdelay"))
		props->bidi_application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
}

static int set_channel_conf(const struct media_codec *codec, a2dp_opus_05_t *caps, const struct props *props)
{
	/*
	 * Predefined codec profiles
	 */
	if (caps->main.channels < 1)
		return -EINVAL;

	caps->main.coupled_streams = 0;
	OPUS_05_SET_LOCATION(caps->main, 0);

	caps->bidi.coupled_streams = 0;
	OPUS_05_SET_LOCATION(caps->bidi, 0);

	switch (codec->id) {
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05:
		caps->main.channels = SPA_MIN(2, caps->main.channels);
		if (caps->main.channels == 2) {
			caps->main.coupled_streams = surround_encoders[1].coupled_streams;
			OPUS_05_SET_LOCATION(caps->main, surround_encoders[1].location);
		}
		caps->bidi.channels = 0;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_51:
		if (caps->main.channels < 6)
			return -EINVAL;
		caps->main.channels = surround_encoders[5].channels;
		caps->main.coupled_streams = surround_encoders[5].coupled_streams;
		OPUS_05_SET_LOCATION(caps->main, surround_encoders[5].location);
		caps->bidi.channels = 0;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_71:
		if (caps->main.channels < 8)
			return -EINVAL;
		caps->main.channels = surround_encoders[7].channels;
		caps->main.coupled_streams = surround_encoders[7].coupled_streams;
		OPUS_05_SET_LOCATION(caps->main, surround_encoders[7].location);
		caps->bidi.channels = 0;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_DUPLEX:
		if (caps->bidi.channels < 1)
			return -EINVAL;
		caps->main.channels = SPA_MIN(2, caps->main.channels);
		if (caps->main.channels == 2) {
			caps->main.coupled_streams = surround_encoders[1].coupled_streams;
			OPUS_05_SET_LOCATION(caps->main, surround_encoders[1].location);
		}
		caps->bidi.channels = SPA_MIN(2, caps->bidi.channels);
		if (caps->bidi.channels == 2) {
			caps->bidi.coupled_streams = surround_encoders[1].coupled_streams;
			OPUS_05_SET_LOCATION(caps->bidi, surround_encoders[1].location);
		}
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO:
		if (caps->main.channels < props->channels)
			return -EINVAL;
		if (props->bidi_channels == 0 && caps->bidi.channels != 0)
			return -EINVAL;
		if (caps->bidi.channels < props->bidi_channels)
			return -EINVAL;
		caps->main.channels = props->channels;
		caps->main.coupled_streams = props->coupled_streams;
		OPUS_05_SET_LOCATION(caps->main, props->location);
		caps->bidi.channels = props->bidi_channels;
		caps->bidi.coupled_streams = props->bidi_coupled_streams;
		OPUS_05_SET_LOCATION(caps->bidi, props->bidi_location);
		break;
	default:
		spa_assert(false);
	};

	return 0;
}

static void get_default_bitrates(const struct media_codec *codec, bool bidi, int *min, int *max, int *init)
{
	int tmp;

	if (min == NULL)
		min = &tmp;
	if (max == NULL)
		max = &tmp;
	if (init == NULL)
		init = &tmp;

	if (bidi) {
		*min = SPA_MIN(BITRATE_MIN, BITRATE_DUPLEX_BIDI);
		*max = BITRATE_DUPLEX_BIDI;
		*init = BITRATE_DUPLEX_BIDI;
		return;
	}

	switch (codec->id) {
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05:
		*min = BITRATE_MIN;
		*max = BITRATE_MAX;
		*init = BITRATE_INITIAL;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_51:
		*min = BITRATE_MIN_51;
		*max = BITRATE_MAX_51;
		*init = BITRATE_INITIAL_51;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_71:
		*min = BITRATE_MIN_71;
		*max = BITRATE_MAX_71;
		*init = BITRATE_INITIAL_71;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_DUPLEX:
		*min = BITRATE_MIN;
		*max = BITRATE_MAX;
		*init = BITRATE_INITIAL;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO:
	default:
		spa_assert_not_reached();
	};
}

static int get_mapping(const struct media_codec *codec, const a2dp_opus_05_direction_t *conf,
		bool use_surround_encoder, uint8_t *streams_ret, uint8_t *coupled_streams_ret,
		const uint8_t **surround_mapping, uint32_t *positions)
{
	const uint8_t channels = conf->channels;
	const uint32_t location = OPUS_05_GET_LOCATION(*conf);
	const uint8_t coupled_streams = conf->coupled_streams;
	const uint8_t *permutation = NULL;
	size_t i, j;

	if (channels > SPA_AUDIO_MAX_CHANNELS)
		return -EINVAL;
	if (2 * coupled_streams > channels)
		return -EINVAL;

	if (streams_ret)
		*streams_ret = channels - coupled_streams;
	if (coupled_streams_ret)
		*coupled_streams_ret = coupled_streams;

	if (channels == 0)
		return 0;

	if (use_surround_encoder) {
		/* Opus surround encoder supports only some channel configurations, and
		 * needs a specific input channel ordering */
		for (i = 0; i < SPA_N_ELEMENTS(surround_encoders); ++i) {
			const struct surround_encoder_mapping *m = &surround_encoders[i];

			if (m->channels == channels &&
					m->coupled_streams == coupled_streams &&
					m->location == location)
			{
				spa_assert(channels <= SPA_N_ELEMENTS(m->inv_mapping));
				permutation = m->inv_mapping;
				if (surround_mapping)
					*surround_mapping = m->mapping;
				break;
			}
		}
		if (permutation == NULL && surround_mapping)
			*surround_mapping = NULL;
	}

	if (positions) {
		for (i = 0, j = 0; i < SPA_N_ELEMENTS(audio_locations) && j < channels; ++i) {
			const struct audio_location loc = audio_locations[i];

			if (location & loc.mask) {
				if (permutation)
					positions[permutation[j++]] = loc.position;
				else
					positions[j++] = loc.position;
			}
		}
		for (i = SPA_AUDIO_CHANNEL_START_Aux; j < channels; ++i, ++j)
			positions[j] = i;
	}

	return 0;
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	a2dp_opus_05_t a2dp_opus_05 = {
		.info = codec->vendor,
		.main = {
			.channels = SPA_AUDIO_MAX_CHANNELS,
			.frame_duration = (OPUS_05_FRAME_DURATION_25 |
					OPUS_05_FRAME_DURATION_50 |
					OPUS_05_FRAME_DURATION_100 |
					OPUS_05_FRAME_DURATION_200 |
					OPUS_05_FRAME_DURATION_400),
			OPUS_05_INIT_LOCATION(BT_AUDIO_LOCATION_ANY)
			OPUS_05_INIT_BITRATE(0)
		},
		.bidi = {
			.channels = SPA_AUDIO_MAX_CHANNELS,
			.frame_duration = (OPUS_05_FRAME_DURATION_25 |
					OPUS_05_FRAME_DURATION_50 |
					OPUS_05_FRAME_DURATION_100 |
					OPUS_05_FRAME_DURATION_200 |
					OPUS_05_FRAME_DURATION_400),
			OPUS_05_INIT_LOCATION(BT_AUDIO_LOCATION_ANY)
			OPUS_05_INIT_BITRATE(0)
		}
	};

	/* Only duplex/pro codec has bidi, since bluez5-device has to know early
	 * whether to show nodes or not. */
	if (codec->id != SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_DUPLEX &&
			codec->id != SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO)
		spa_zero(a2dp_opus_05.bidi);

	memcpy(caps, &a2dp_opus_05, sizeof(a2dp_opus_05));
	return sizeof(a2dp_opus_05);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	struct props props;
	a2dp_opus_05_t conf;
	int res;
	int max;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	parse_settings(&props, global_settings);

	/* Channel Configuration & Audio Location */
	if ((res = set_channel_conf(codec, &conf, &props)) < 0)
		return res;

	/* Limits */
	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO) {
		max = props.max_bitrate;
		if (OPUS_05_GET_BITRATE(conf.main) != 0)
			OPUS_05_SET_BITRATE(conf.main, SPA_MIN(OPUS_05_GET_BITRATE(conf.main), max / 1024));
		else
			OPUS_05_SET_BITRATE(conf.main, max / 1024);

		max = props.bidi_max_bitrate;
		if (OPUS_05_GET_BITRATE(conf.bidi) != 0)
			OPUS_05_SET_BITRATE(conf.bidi, SPA_MIN(OPUS_05_GET_BITRATE(conf.bidi), max / 1024));
		else
			OPUS_05_SET_BITRATE(conf.bidi, max / 1024);

		if (conf.main.frame_duration & props.frame_duration)
			conf.main.frame_duration = props.frame_duration;
		else
			return -EINVAL;

		if (conf.bidi.channels == 0)
			true;
		else if (conf.bidi.frame_duration & props.bidi_frame_duration)
			conf.bidi.frame_duration = props.bidi_frame_duration;
		else
			return -EINVAL;
	} else {
		if (conf.main.frame_duration & OPUS_05_FRAME_DURATION_100)
			conf.main.frame_duration = OPUS_05_FRAME_DURATION_100;
		else if (conf.main.frame_duration & OPUS_05_FRAME_DURATION_200)
			conf.main.frame_duration = OPUS_05_FRAME_DURATION_200;
		else if (conf.main.frame_duration & OPUS_05_FRAME_DURATION_400)
			conf.main.frame_duration = OPUS_05_FRAME_DURATION_400;
		else if (conf.main.frame_duration & OPUS_05_FRAME_DURATION_50)
			conf.main.frame_duration = OPUS_05_FRAME_DURATION_50;
		else if (conf.main.frame_duration & OPUS_05_FRAME_DURATION_25)
			conf.main.frame_duration = OPUS_05_FRAME_DURATION_25;
		else
			return -EINVAL;

		get_default_bitrates(codec, false, NULL, &max, NULL);

		if (OPUS_05_GET_BITRATE(conf.main) != 0)
			OPUS_05_SET_BITRATE(conf.main, SPA_MIN(OPUS_05_GET_BITRATE(conf.main), max / 1024));
		else
			OPUS_05_SET_BITRATE(conf.main, max / 1024);

		/* longer bidi frames appear to work better */
		if (conf.bidi.channels == 0)
			true;
		else if (conf.bidi.frame_duration & OPUS_05_FRAME_DURATION_200)
			conf.bidi.frame_duration = OPUS_05_FRAME_DURATION_200;
		else if (conf.bidi.frame_duration & OPUS_05_FRAME_DURATION_100)
			conf.bidi.frame_duration = OPUS_05_FRAME_DURATION_100;
		else if (conf.bidi.frame_duration & OPUS_05_FRAME_DURATION_400)
			conf.bidi.frame_duration = OPUS_05_FRAME_DURATION_400;
		else if (conf.bidi.frame_duration & OPUS_05_FRAME_DURATION_50)
			conf.bidi.frame_duration = OPUS_05_FRAME_DURATION_50;
		else if (conf.bidi.frame_duration & OPUS_05_FRAME_DURATION_25)
			conf.bidi.frame_duration = OPUS_05_FRAME_DURATION_25;
		else
			return -EINVAL;

		get_default_bitrates(codec, true, NULL, &max, NULL);

		if (conf.bidi.channels == 0)
			true;
		else if (OPUS_05_GET_BITRATE(conf.bidi) != 0)
			OPUS_05_SET_BITRATE(conf.bidi, SPA_MIN(OPUS_05_GET_BITRATE(conf.bidi), max / 1024));
		else
			OPUS_05_SET_BITRATE(conf.bidi, max / 1024);
	}

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings)
{
	a2dp_opus_05_t conf1, conf2, cap1, cap2;
	a2dp_opus_05_t *conf;
	int res1, res2;
	int a, b;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, flags, caps1, caps1_size, info, global_settings, (uint8_t *)&conf1);
	res2 = codec->select_config(codec, flags, caps2, caps2_size, info, global_settings, (uint8_t *)&conf2);

#define PREFER_EXPR(expr)			\
		do {				\
			conf = &conf1; 		\
			a = (expr);		\
			conf = &conf2;		\
			b = (expr);		\
			if (a != b)		\
				return b - a;	\
		} while (0)

#define PREFER_BOOL(expr)	PREFER_EXPR((expr) ? 1 : 0)

	/* Prefer valid */
	a = (res1 > 0 && (size_t)res1 == sizeof(a2dp_opus_05_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(a2dp_opus_05_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	memcpy(&cap1, caps1, sizeof(cap1));
	memcpy(&cap2, caps2, sizeof(cap2));

	if (conf1.bidi.channels == 0 && conf2.bidi.channels == 0) {
		/* If no bidi, prefer the SEP that has none */
		a = (cap1.bidi.channels == 0);
		b = (cap2.bidi.channels == 0);
		if (a != b)
			return b - a;
	}

	PREFER_EXPR(conf->main.channels);
	PREFER_EXPR(conf->bidi.channels);
	PREFER_EXPR(OPUS_05_GET_BITRATE(conf->main));
	PREFER_EXPR(OPUS_05_GET_BITRATE(conf->bidi));

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static bool is_duplex_codec(const struct media_codec *codec)
{
	return codec->id == 0;
}

static bool use_surround_encoder(const struct media_codec *codec, bool is_sink)
{
	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO)
		return false;

	if (is_duplex_codec(codec))
		return is_sink;
	else
		return !is_sink;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	const bool surround_encoder = use_surround_encoder(codec, flags & MEDIA_CODEC_FLAG_SINK);
	a2dp_opus_05_t conf;
	a2dp_opus_05_direction_t *dir;
	struct spa_pod_frame f[1];
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	dir = !is_duplex_codec(codec) ? &conf.main : &conf.bidi;

	if (get_mapping(codec, dir, surround_encoder, NULL, NULL, NULL, position) < 0)
		return -EINVAL;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_ENUM_Int(6,
					48000, 48000, 24000, 16000, 12000, 8000),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(dir->channels),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, dir->channels, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	const bool surround_encoder = use_surround_encoder(codec, flags & MEDIA_CODEC_FLAG_SINK);
	const a2dp_opus_05_direction_t *dir1, *dir2;
	const a2dp_opus_05_t *conf;

	if (caps == NULL || caps_size < sizeof(*conf))
		return -EINVAL;

	conf = caps;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_F32;
	info->info.raw.rate = 0;  /* not specified by config */

	if (2 * conf->main.coupled_streams > conf->main.channels)
		return -EINVAL;
	if (2 * conf->bidi.coupled_streams > conf->bidi.channels)
		return -EINVAL;

	if (!is_duplex_codec(codec)) {
		dir1 = &conf->main;
		dir2 = &conf->bidi;
	} else {
		dir1 = &conf->bidi;
		dir2 = &conf->main;
	}

	info->info.raw.channels = dir1->channels;
	if (get_mapping(codec, dir1, surround_encoder, NULL, NULL, NULL, info->info.raw.position) < 0)
		return -EINVAL;
	if (get_mapping(codec, dir2, surround_encoder, NULL, NULL, NULL, NULL) < 0)
		return -EINVAL;

	return 0;
}

static size_t ceildiv(size_t v, size_t divisor)
{
	if (v % divisor == 0)
		return v / divisor;
	else
		return v / divisor + 1;
}

static bool check_bitrate_vs_frame_dms(struct impl *this, size_t bitrate)
{
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	size_t max_fragments = 0xf;
	size_t payload_size = BUFSIZE_FROM_BITRATE(bitrate, this->e.frame_dms);
	return (size_t)this->mtu >= header_size + ceildiv(payload_size, max_fragments);
}

static int parse_frame_dms(int bitfield)
{
	switch (bitfield) {
	case OPUS_05_FRAME_DURATION_25:
		return 25;
	case OPUS_05_FRAME_DURATION_50:
		return 50;
	case OPUS_05_FRAME_DURATION_100:
		return 100;
	case OPUS_05_FRAME_DURATION_200:
		return 200;
	case OPUS_05_FRAME_DURATION_400:
		return 400;
	default:
		return -EINVAL;
	}
}

static void *codec_init_props(const struct media_codec *codec, uint32_t flags, const struct spa_dict *settings)
{
	struct props *p;

	if (codec->id != SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO)
		return NULL;

	p = calloc(1, sizeof(struct props));
	if (p == NULL)
		return NULL;

	parse_settings(p, settings);

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
	const bool surround_encoder = use_surround_encoder(codec, flags & MEDIA_CODEC_FLAG_SINK);
	a2dp_opus_05_t *conf = config;
	a2dp_opus_05_direction_t *dir;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	const uint8_t *enc_mapping = NULL;
	unsigned char mapping[256];
	size_t i;
	int res;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_F32) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	this->is_bidi = is_duplex_codec(codec);
	dir = !this->is_bidi ? &conf->main : &conf->bidi;

	if ((res = codec_validate_config(codec, flags, config, config_len, &config_info)) < 0)
		goto error;
	if ((res = get_mapping(codec, dir, surround_encoder, &this->streams, &this->coupled_streams,
							&enc_mapping, NULL)) < 0)
		goto error;
	if (config_info.info.raw.channels != info->info.raw.channels) {
		res = -EINVAL;
		goto error;
	}

	this->mtu = mtu;
	this->samplerate = info->info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->application = OPUS_APPLICATION_AUDIO;

	if (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO && props) {
		struct props *p = props;
		this->application = !this->is_bidi ? p->application :
			p->bidi_application;
	}

	/*
	 * Setup encoder
	 */
	if (enc_mapping) {
		int streams, coupled_streams;
		bool incompatible_opus_surround_encoder = false;

		this->enc = opus_multistream_surround_encoder_create(
				this->samplerate, this->channels, 1, &streams, &coupled_streams,
				mapping, this->application, &res);

		if (this->enc) {
			/* Check surround encoder channel mapping is what we want */
			if (streams != this->streams || coupled_streams != this->coupled_streams)
				incompatible_opus_surround_encoder = true;
			for (i = 0; i < this->channels; ++i)
				if (enc_mapping[i] != mapping[i])
					incompatible_opus_surround_encoder = true;
		}

		/* Assert: this should never happen */
		spa_assert(!incompatible_opus_surround_encoder);
		if (incompatible_opus_surround_encoder) {
			res = -EINVAL;
			goto error;
		}
	} else {
		for (i = 0; i < this->channels; ++i)
			mapping[i] = i;
		this->enc = opus_multistream_encoder_create(
				this->samplerate, this->channels, this->streams, this->coupled_streams,
				mapping, this->application, &res);
	}
	if (this->enc == NULL) {
		res = -EINVAL;
		goto error;
	}

	if ((this->e.frame_dms = parse_frame_dms(dir->frame_duration)) < 0) {
		res = -EINVAL;
		goto error;
	}

	if (codec->id != SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO) {
		get_default_bitrates(codec, this->is_bidi, &this->e.bitrate_min,
				&this->e.bitrate_max, &this->e.bitrate);
		this->e.bitrate_max = SPA_MIN(this->e.bitrate_max,
				OPUS_05_GET_BITRATE(*dir) * 1024);
	} else {
		this->e.bitrate_max = OPUS_05_GET_BITRATE(*dir) * 1024;
		this->e.bitrate_min = BITRATE_MIN;
		this->e.bitrate = BITRATE_INITIAL;
	}

	this->e.bitrate_min = SPA_MIN(this->e.bitrate_min, this->e.bitrate_max);
	this->e.bitrate = SPA_CLAMP(this->e.bitrate, this->e.bitrate_min, this->e.bitrate_max);

	this->e.next_bitrate = this->e.bitrate;
	opus_multistream_encoder_ctl(this->enc, OPUS_SET_BITRATE(this->e.bitrate));

	this->e.samples = this->e.frame_dms * this->samplerate / 10000;
	this->e.codesize = this->e.samples * (int)this->channels * sizeof(float);


	/*
	 * Setup decoder
	 */
	for (i = 0; i < this->channels; ++i)
		mapping[i] = i;
	this->dec = opus_multistream_decoder_create(
			this->samplerate, this->channels,
			this->streams, this->coupled_streams,
			mapping, &res);
	if (this->dec == NULL) {
		res = -EINVAL;
		goto error;
	}

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this && this->enc)
		opus_multistream_encoder_destroy(this->enc);
	if (this && this->dec)
		opus_multistream_decoder_destroy(this->dec);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	opus_multistream_encoder_destroy(this->enc);
	opus_multistream_decoder_destroy(this->dec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->e.codesize;
}

static int codec_update_bitrate(struct impl *this)
{
	this->e.next_bitrate = SPA_CLAMP(this->e.next_bitrate,
			this->e.bitrate_min, this->e.bitrate_max);

	if (!check_bitrate_vs_frame_dms(this, this->e.next_bitrate)) {
		this->e.next_bitrate = this->e.bitrate;
		return 0;
	}

	this->e.bitrate = this->e.next_bitrate;
	opus_multistream_encoder_ctl(this->enc, OPUS_SET_BITRATE(this->e.bitrate));
	return 0;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	if (dst_size <= header_size)
		return -EINVAL;

	codec_update_bitrate(this);

	this->e.header = (struct rtp_header *)dst;
	this->e.payload = SPA_PTROFF(dst, sizeof(struct rtp_header), struct rtp_payload);
	memset(dst, 0, header_size);

	this->e.payload->frame_count = 0;
	this->e.header->v = 2;
	this->e.header->pt = 96;
	this->e.header->sequence_number = htons(seqnum);
	this->e.header->timestamp = htonl(timestamp);
	this->e.header->ssrc = htonl(1);

	this->e.packet_size = header_size;
	return this->e.packet_size;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	const int header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	int size;
	int res;

	if (src == NULL) {
		/* Produce fragment packets.
		 *
		 * We assume the caller gives the same buffer here as in the previous
		 * calls to encode(), without changes in the buffer content.
		 */
		if (this->e.fragment == NULL ||
				this->e.fragment_count <= 1 ||
				this->e.fragment < dst ||
				SPA_PTROFF(this->e.fragment, this->e.fragment_size, void) > SPA_PTROFF(dst, dst_size, void)) {
			this->e.fragment = NULL;
			return -EINVAL;
		}

		size = SPA_MIN(this->mtu - header_size, this->e.fragment_size);
		memmove(dst, this->e.fragment, size);
		*dst_out = size;

		this->e.payload->is_fragmented = 1;
		this->e.payload->frame_count = --this->e.fragment_count;
		this->e.payload->is_last_fragment = (this->e.fragment_count == 1);

		if (this->e.fragment_size > size && this->e.fragment_count > 1) {
			this->e.fragment = SPA_PTROFF(this->e.fragment, size, void);
			this->e.fragment_size -= size;
			*need_flush = NEED_FLUSH_FRAGMENT;
		} else {
			this->e.fragment = NULL;
			*need_flush = NEED_FLUSH_ALL;
		}
		return 0;
	}

	if (src_size < (size_t)this->e.codesize) {
		*dst_out = 0;
		return 0;
	}

	res = opus_multistream_encode_float(
		this->enc, src, this->e.samples, dst, dst_size);
	if (res < 0)
		return -EINVAL;
	*dst_out = res;

	this->e.packet_size += res;
	this->e.payload->frame_count++;

	if (this->e.packet_size > this->mtu) {
		/* Fragment packet */
		this->e.fragment_count = ceildiv(this->e.packet_size - header_size,
				this->mtu - header_size);

		this->e.payload->is_fragmented = 1;
		this->e.payload->is_first_fragment = 1;
		this->e.payload->frame_count = this->e.fragment_count;

		this->e.fragment_size = this->e.packet_size - this->mtu;
		this->e.fragment = SPA_PTROFF(dst, *dst_out - this->e.fragment_size, void);
		*need_flush = NEED_FLUSH_FRAGMENT;

		/*
		 * We keep the rest of the encoded frame in the same buffer, and rely
		 * that the caller won't overwrite it before the next call to encode()
		 */
		*dst_out = SPA_PTRDIFF(this->e.fragment, dst);
	} else {
		*need_flush = NEED_FLUSH_ALL;
	}

	return this->e.codesize;
}

static SPA_UNUSED int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;
	const struct rtp_header *header = src;
	const struct rtp_payload *payload = SPA_PTROFF(src, sizeof(struct rtp_header), void);
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	spa_return_val_if_fail (src_size > header_size, -EINVAL);

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	if (payload->is_fragmented) {
		if (payload->is_first_fragment) {
			this->d.fragment_size = 0;
		} else if (payload->frame_count + 1 != this->d.fragment_count ||
				(payload->frame_count == 1 && !payload->is_last_fragment)){
			/* Fragments not in right order: drop packet */
			return -EINVAL;
		}
		this->d.fragment_count = payload->frame_count;
	} else {
		if (payload->frame_count != 1)
			return -EINVAL;
		this->d.fragment_count = 0;
	}

	return header_size;
}

static SPA_UNUSED int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int consumed = src_size;
	int res;
	int dst_samples;

	if (this->d.fragment_count > 0) {
		/* Fragmented frame */
		size_t avail;
		avail = SPA_MIN(sizeof(this->d.fragment) - this->d.fragment_size, src_size);
		memcpy(SPA_PTROFF(this->d.fragment, this->d.fragment_size, void), src, avail);

		this->d.fragment_size += avail;

		if (this->d.fragment_count > 1) {
			/* More fragments to come */
			*dst_out = 0;
			return consumed;
		}

		src = this->d.fragment;
		src_size = this->d.fragment_size;

		this->d.fragment_count = 0;
		this->d.fragment_size = 0;
	}

	dst_samples = dst_size / (sizeof(float) * this->channels);
	res = opus_multistream_decode_float(this->dec, src, src_size, dst, dst_samples, 0);
	if (res < 0)
		return -EINVAL;
	*dst_out = (size_t)res * this->channels * sizeof(float);

	return consumed;
}

static int codec_abr_process(void *data, size_t unsent)
{
	const uint64_t interval = SPA_NSEC_PER_SEC;
	struct impl *this = data;
	struct abr *abr = &this->e.abr;
	bool level_bad, level_good;
	uint32_t actual_bitrate;

	abr->total_size += this->e.packet_size;

	if (this->e.payload->is_fragmented && !this->e.payload->is_first_fragment)
		return 0;

	abr->now += this->e.frame_dms * SPA_NSEC_PER_MSEC / 10;

	abr->buffer_level = SPA_MAX(abr->buffer_level, unsent);
	abr->packet_size = SPA_MAX(abr->packet_size, (uint32_t)this->e.packet_size);
	abr->packet_size = SPA_MAX(abr->packet_size, 128u);

	level_bad = abr->buffer_level > 2 * (uint32_t)this->mtu || abr->bad;
	level_good = abr->buffer_level == 0;

	if (!(abr->last_update + interval <= abr->now ||
			(level_bad && abr->last_change + interval <= abr->now)))
		return 0;

	actual_bitrate = (uint64_t)abr->total_size*8*SPA_NSEC_PER_SEC
		/ SPA_MAX(1u, abr->now - abr->last_update);

	spa_log_debug(log, "opus ABR bitrate:%d actual:%d level:%d (%s) bad:%d retry:%ds size:%d",
			(int)this->e.bitrate,
			(int)actual_bitrate,
			(int)abr->buffer_level,
			level_bad ? "bad" : (level_good ? "good" : "-"),
			(int)abr->bad,
			(int)(abr->retry_interval / SPA_NSEC_PER_SEC),
			(int)abr->packet_size);

	if (level_bad) {
		this->e.next_bitrate = this->e.bitrate * 11 / 12;
		abr->last_change = abr->now;
		abr->retry_interval = SPA_MIN(abr->retry_interval + 10*interval,
				30 * interval);
	} else if (!level_good) {
		abr->last_change = abr->now;
	} else if (abr->now < abr->last_change + abr->retry_interval) {
		/* noop */
	} else if (actual_bitrate*3/2 < (uint32_t)this->e.bitrate) {
		/* actual bitrate is small compared to target; probably silence */
	} else {
		this->e.next_bitrate = this->e.bitrate
			+ SPA_MAX(1, this->e.bitrate_max / 40);
		abr->last_change = abr->now;
		abr->retry_interval = SPA_MAX(abr->retry_interval, (5+4)*interval)
			- 4*interval;
	}

	abr->last_update = abr->now;
	abr->buffer_level = 0;
	abr->bad = false;
	abr->packet_size = 0;
	abr->total_size = 0;

	return 0;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;
	struct abr *abr = &this->e.abr;
	abr->bad = true;
	return 0;
}

static int codec_increase_bitpool(void *data)
{
	return 0;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &log_topic);
}

#define OPUS_05_COMMON_DEFS					\
	.codec_id = A2DP_CODEC_VENDOR,				\
	.vendor = { .vendor_id = OPUS_05_VENDOR_ID,		\
			.codec_id = OPUS_05_CODEC_ID },		\
	.select_config = codec_select_config,			\
	.enum_config = codec_enum_config,			\
	.validate_config = codec_validate_config,		\
	.caps_preference_cmp = codec_caps_preference_cmp,	\
	.init = codec_init,					\
	.deinit = codec_deinit,					\
	.get_block_size = codec_get_block_size,			\
	.abr_process = codec_abr_process,			\
	.start_encode = codec_start_encode,			\
	.encode = codec_encode,					\
	.reduce_bitpool = codec_reduce_bitpool,			\
	.increase_bitpool = codec_increase_bitpool,		\
	.set_log = codec_set_log

#define OPUS_05_COMMON_FULL_DEFS				\
	OPUS_05_COMMON_DEFS,					\
	.start_decode = codec_start_decode,			\
	.decode = codec_decode

const struct media_codec a2dp_codec_opus_05 = {
	OPUS_05_COMMON_FULL_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05,
	.name = "opus_05",
	.description = "Opus",
	.fill_caps = codec_fill_caps,
};

const struct media_codec a2dp_codec_opus_05_51 = {
	OPUS_05_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_51,
	.name = "opus_05_51",
	.description = "Opus 5.1 Surround",
	.endpoint_name = "opus_05",
	.fill_caps = NULL,
};

const struct media_codec a2dp_codec_opus_05_71 = {
	OPUS_05_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_71,
	.name = "opus_05_71",
	.description = "Opus 7.1 Surround",
	.endpoint_name = "opus_05",
	.fill_caps = NULL,
};

/* Bidi return channel codec: doesn't have endpoints */
const struct media_codec a2dp_codec_opus_05_return = {
	OPUS_05_COMMON_FULL_DEFS,
	.id = 0,
	.name = "opus_05_duplex_bidi",
	.description = "Opus Duplex Bidi channel",
};

const struct media_codec a2dp_codec_opus_05_duplex = {
	OPUS_05_COMMON_FULL_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_DUPLEX,
	.name = "opus_05_duplex",
	.description = "Opus Duplex",
	.duplex_codec = &a2dp_codec_opus_05_return,
	.fill_caps = codec_fill_caps,
};

const struct media_codec a2dp_codec_opus_05_pro = {
	OPUS_05_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_05_PRO,
	.name = "opus_05_pro",
	.description = "Opus Pro Audio",
	.init_props = codec_init_props,
	.clear_props = codec_clear_props,
	.duplex_codec = &a2dp_codec_opus_05_return,
	.endpoint_name = "opus_05_duplex",
	.fill_caps = NULL,
};

MEDIA_CODEC_EXPORT_DEF(
	"opus",
	&a2dp_codec_opus_05,
	&a2dp_codec_opus_05_51,
	&a2dp_codec_opus_05_71,
	&a2dp_codec_opus_05_duplex,
	&a2dp_codec_opus_05_pro
);
