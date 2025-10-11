/* Spa BAP codec API */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_BAP_CODEC_CAPS_H_
#define SPA_BLUEZ5_BAP_CODEC_CAPS_H_

#include <spa/param/audio/format.h>

#define BAP_CODEC_LC3           0x06

#define LC3_TYPE_FREQ           0x01
#define LC3_FREQ_8KHZ           (1 << 0)
#define LC3_FREQ_11KHZ          (1 << 1)
#define LC3_FREQ_16KHZ          (1 << 2)
#define LC3_FREQ_22KHZ          (1 << 3)
#define LC3_FREQ_24KHZ          (1 << 4)
#define LC3_FREQ_32KHZ          (1 << 5)
#define LC3_FREQ_44KHZ          (1 << 6)
#define LC3_FREQ_48KHZ          (1 << 7)
#define LC3_FREQ_ANY            (LC3_FREQ_8KHZ | \
                                 LC3_FREQ_11KHZ | \
                                 LC3_FREQ_16KHZ | \
                                 LC3_FREQ_22KHZ | \
                                 LC3_FREQ_24KHZ | \
                                 LC3_FREQ_32KHZ | \
                                 LC3_FREQ_44KHZ | \
                                 LC3_FREQ_48KHZ)

#define LC3_VAL_FREQ_8KHZ       8000
#define LC3_VAL_FREQ_11KHZ      11025
#define LC3_VAL_FREQ_16KHZ      16000
#define LC3_VAL_FREQ_22KHZ      22050
#define LC3_VAL_FREQ_24KHZ      24000
#define LC3_VAL_FREQ_32KHZ      32000
#define LC3_VAL_FREQ_44KHZ      44100
#define LC3_VAL_FREQ_48KHZ      48000

#define LC3_TYPE_DUR            0x02
#define LC3_DUR_7_5             (1 << 0)
#define LC3_DUR_10              (1 << 1)
#define LC3_DUR_ANY             (LC3_DUR_7_5 | \
                                 LC3_DUR_10)

#define LC3_VAL_DUR_7_5         7.5
#define LC3_VAL_DUR_10          10

#define LC3_TYPE_CHAN           0x03
#define LC3_CHAN_1              (1 << 0)
#define LC3_CHAN_2              (1 << 1)
#define LC3_CHAN_3              (1 << 2)
#define LC3_CHAN_4              (1 << 3)
#define LC3_CHAN_5              (1 << 4)
#define LC3_CHAN_6              (1 << 5)
#define LC3_CHAN_7              (1 << 6)
#define LC3_CHAN_8              (1 << 7)

#define LC3_VAL_CHAN_1          1
#define LC3_VAL_CHAN_2          2
#define LC3_VAL_CHAN_3          3
#define LC3_VAL_CHAN_4          4
#define LC3_VAL_CHAN_5          5
#define LC3_VAL_CHAN_6          6
#define LC3_VAL_CHAN_7          7
#define LC3_VAL_CHAN_8          8

#define LC3_TYPE_FRAMELEN       0x04
#define LC3_TYPE_BLKS           0x05

/* LC3 config parameters */
#define LC3_CONFIG_FREQ_8KHZ    0x01
#define LC3_CONFIG_FREQ_11KHZ   0x02
#define LC3_CONFIG_FREQ_16KHZ   0x03
#define LC3_CONFIG_FREQ_22KHZ   0x04
#define LC3_CONFIG_FREQ_24KHZ   0x05
#define LC3_CONFIG_FREQ_32KHZ   0x06
#define LC3_CONFIG_FREQ_44KHZ   0x07
#define LC3_CONFIG_FREQ_48KHZ   0x08

#define LC3_CONFIG_DURATION_7_5 0x00
#define LC3_CONFIG_DURATION_10  0x01

#define LC3_MAX_CHANNELS 28

#define BAP_CHANNEL_MONO	0x00000000 /* mono */
#define BAP_CHANNEL_FL		0x00000001 /* front left */
#define BAP_CHANNEL_FR		0x00000002 /* front right */
#define BAP_CHANNEL_FC		0x00000004 /* front center */
#define BAP_CHANNEL_LFE		0x00000008 /* LFE */
#define BAP_CHANNEL_BL		0x00000010 /* back left */
#define BAP_CHANNEL_BR		0x00000020 /* back right */
#define BAP_CHANNEL_FLC		0x00000040 /* front left center */
#define BAP_CHANNEL_FRC		0x00000080 /* front right center */
#define BAP_CHANNEL_BC		0x00000100 /* back center */
#define BAP_CHANNEL_LFE2	0x00000200 /* LFE 2 */
#define BAP_CHANNEL_SL		0x00000400 /* side left */
#define BAP_CHANNEL_SR		0x00000800 /* side right */
#define BAP_CHANNEL_TFL		0x00001000 /* top front left */
#define BAP_CHANNEL_TFR		0x00002000 /* top front right */
#define BAP_CHANNEL_TFC		0x00004000 /* top front center */
#define BAP_CHANNEL_TC		0x00008000 /* top center */
#define BAP_CHANNEL_TBL		0x00010000 /* top back left */
#define BAP_CHANNEL_TBR		0x00020000 /* top back right */
#define BAP_CHANNEL_TSL		0x00040000 /* top side left */
#define BAP_CHANNEL_TSR		0x00080000 /* top side right */
#define BAP_CHANNEL_TBC		0x00100000 /* top back center */
#define BAP_CHANNEL_BFC		0x00200000 /* bottom front center */
#define BAP_CHANNEL_BFL		0x00400000 /* bottom front left */
#define BAP_CHANNEL_BFR		0x00800000 /* bottom front right */
#define BAP_CHANNEL_FLW		0x01000000 /* front left wide */
#define BAP_CHANNEL_FRW		0x02000000 /* front right wide */
#define BAP_CHANNEL_LS		0x04000000 /* left surround */
#define BAP_CHANNEL_RS		0x08000000 /* right surround */

#define BAP_CHANNEL_ALL         0x0fffffff /* mask of all */

#define BAP_CONTEXT_PROHIBITED		0x0000 /* Prohibited */
#define BAP_CONTEXT_UNSPECIFIED		0x0001 /* Unspecified */
#define BAP_CONTEXT_CONVERSATIONAL	0x0002 /* Telephony, video calls, ... */
#define BAP_CONTEXT_MEDIA		0x0004 /* Music, radio, podcast, movie soundtrack, TV */
#define BAP_CONTEXT_GAME		0x0008 /* Gaming media, game effects, music, in-game voice chat  */
#define BAP_CONTEXT_INSTRUCTIONAL	0x0010 /* Instructional audio, navigation, announcements, user guidance */
#define BAP_CONTEXT_VOICE		0x0020 /* Man-machine communication, voice recognition, virtual assistants */
#define BAP_CONTEXT_LIVE		0x0040 /* Live audio, perceived both via direct acoustic path and via BAP */
#define BAP_CONTEXT_SOUND_EFFECTS	0x0080 /* Keyboard and touch feedback, menu, UI, other system sounds */
#define BAP_CONTEXT_NOTIFICATIONS	0x0100 /* Attention-seeking, message arrival, reminders */
#define BAP_CONTEXT_RINGTONE		0x0200 /* Incoming call alert audio */
#define BAP_CONTEXT_ALERTS		0x0400 /* Alarms and timers, critical battery, alarm clock, toaster */
#define BAP_CONTEXT_EMERGENCY		0x0800 /* Fire alarm, other urgent alerts */

#define BAP_CONTEXT_ALL			0x0fff

#define BT_ISO_QOS_CIG_UNSET    0xff
#define BT_ISO_QOS_CIS_UNSET    0xff

#define BT_ISO_QOS_TARGET_LATENCY_LOW		0x01
#define BT_ISO_QOS_TARGET_LATENCY_BALANCED	0x02
#define BT_ISO_QOS_TARGET_LATENCY_RELIABILITY	0x03

struct bap_endpoint_qos {
	uint8_t	framing;
	uint8_t	phy;
	uint8_t	retransmission;
	uint16_t latency;
	uint32_t delay_min;
	uint32_t delay_max;
	uint32_t preferred_delay_min;
	uint32_t preferred_delay_max;
	uint32_t locations;
	uint16_t supported_context;
	uint16_t context;
	uint32_t channel_allocation;
};

struct bap_codec_qos {
	uint32_t interval;
	uint8_t framing;
	uint8_t phy;
	uint16_t sdu;
	uint8_t retransmission;
	uint16_t latency;
	uint32_t delay;
	uint8_t target_latency;
};

struct bap_codec_qos_full {
	uint8_t cig;
	uint8_t cis;
	uint8_t big;
	uint8_t bis;
	struct bap_codec_qos qos;
};

static const struct {
	uint32_t bit;
	enum spa_audio_channel channel;
} bap_channel_bits[] = {
	{ BAP_CHANNEL_MONO, SPA_AUDIO_CHANNEL_MONO },
	{ BAP_CHANNEL_FL,   SPA_AUDIO_CHANNEL_FL },
	{ BAP_CHANNEL_FR,   SPA_AUDIO_CHANNEL_FR },
	{ BAP_CHANNEL_FC,   SPA_AUDIO_CHANNEL_FC },
	{ BAP_CHANNEL_LFE,  SPA_AUDIO_CHANNEL_LFE },
	{ BAP_CHANNEL_BL,   SPA_AUDIO_CHANNEL_RL },
	{ BAP_CHANNEL_BR,   SPA_AUDIO_CHANNEL_RR },
	{ BAP_CHANNEL_FLC,  SPA_AUDIO_CHANNEL_FLC },
	{ BAP_CHANNEL_FRC,  SPA_AUDIO_CHANNEL_FRC },
	{ BAP_CHANNEL_BC,   SPA_AUDIO_CHANNEL_BC },
	{ BAP_CHANNEL_LFE2, SPA_AUDIO_CHANNEL_LFE2 },
	{ BAP_CHANNEL_SL,   SPA_AUDIO_CHANNEL_SL },
	{ BAP_CHANNEL_SR,   SPA_AUDIO_CHANNEL_SR },
	{ BAP_CHANNEL_TFL,  SPA_AUDIO_CHANNEL_TFL },
	{ BAP_CHANNEL_TFR,  SPA_AUDIO_CHANNEL_TFR },
	{ BAP_CHANNEL_TFC,  SPA_AUDIO_CHANNEL_TFC },
	{ BAP_CHANNEL_TC,   SPA_AUDIO_CHANNEL_TC },
	{ BAP_CHANNEL_TBL,  SPA_AUDIO_CHANNEL_TRL },
	{ BAP_CHANNEL_TBR,  SPA_AUDIO_CHANNEL_TRR },
	{ BAP_CHANNEL_TSL,  SPA_AUDIO_CHANNEL_TSL },
	{ BAP_CHANNEL_TSR,  SPA_AUDIO_CHANNEL_TSR },
	{ BAP_CHANNEL_TBC,  SPA_AUDIO_CHANNEL_TRC },
	{ BAP_CHANNEL_BFC,  SPA_AUDIO_CHANNEL_BC },
	{ BAP_CHANNEL_BFL,  SPA_AUDIO_CHANNEL_BLC },
	{ BAP_CHANNEL_BFR,  SPA_AUDIO_CHANNEL_BRC },
	{ BAP_CHANNEL_FLW,  SPA_AUDIO_CHANNEL_FLW },
	{ BAP_CHANNEL_FRW,  SPA_AUDIO_CHANNEL_FRW },
	{ BAP_CHANNEL_LS,   SPA_AUDIO_CHANNEL_SL }, /* is it the right mapping? */
	{ BAP_CHANNEL_RS,   SPA_AUDIO_CHANNEL_SR }, /* is it the right mapping? */
};

#endif
