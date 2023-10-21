/* Spa BAP codec API */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_BAP_CODEC_CAPS_H_
#define SPA_BLUEZ5_BAP_CODEC_CAPS_H_

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

#define LC3_TYPE_DUR            0x02
#define LC3_DUR_7_5             (1 << 0)
#define LC3_DUR_10              (1 << 1)
#define LC3_DUR_ANY             (LC3_DUR_7_5 | \
                                 LC3_DUR_10)

#define LC3_TYPE_CHAN           0x03
#define LC3_CHAN_1              (1 << 0)
#define LC3_CHAN_2              (1 << 1)

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

#define BAP_CHANNEL_NOT_ALLOWED	0x00000000
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

struct __attribute__((packed)) ltv {
	uint8_t  len;
	uint8_t  type;
	uint8_t  value[];
};

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

#endif
