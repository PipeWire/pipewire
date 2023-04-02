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

#define LC3_CONFIG_CHNL_NOT_ALLOWED 0x00000000
#define LC3_CONFIG_CHNL_FL          0x00000001 /* front left */
#define LC3_CONFIG_CHNL_FR          0x00000002 /* front right */
#define LC3_CONFIG_CHNL_FC          0x00000004 /* front center */
#define LC3_CONFIG_CHNL_LFE         0x00000008 /* LFE */
#define LC3_CONFIG_CHNL_BL          0x00000010 /* back left */
#define LC3_CONFIG_CHNL_BR          0x00000020 /* back right */
#define LC3_CONFIG_CHNL_FLC         0x00000040 /* front left center */
#define LC3_CONFIG_CHNL_FRC         0x00000080 /* front right center */
#define LC3_CONFIG_CHNL_BC          0x00000100 /* back center */
#define LC3_CONFIG_CHNL_LFE2		0x00000200 /* LFE 2 */
#define LC3_CONFIG_CHNL_SL          0x00000400 /* side left */
#define LC3_CONFIG_CHNL_SR          0x00000800 /* side right */
#define LC3_CONFIG_CHNL_TFL         0x00001000 /* top front left */
#define LC3_CONFIG_CHNL_TFR         0x00002000 /* top front right */
#define LC3_CONFIG_CHNL_TFC         0x00004000 /* top front center */
#define LC3_CONFIG_CHNL_TC          0x00008000 /* top center */
#define LC3_CONFIG_CHNL_TBL         0x00010000 /* top back left */
#define LC3_CONFIG_CHNL_TBR         0x00020000 /* top back right */
#define LC3_CONFIG_CHNL_TSL         0x00040000 /* top side left */
#define LC3_CONFIG_CHNL_TSR         0x00080000 /* top side right */
#define LC3_CONFIG_CHNL_TBC         0x00100000 /* top back center */
#define LC3_CONFIG_CHNL_BFC         0x00200000 /* bottom front center */
#define LC3_CONFIG_CHNL_BFL         0x00400000 /* bottom front left */
#define LC3_CONFIG_CHNL_BFR         0x00800000 /* bottom front right */
#define LC3_CONFIG_CHNL_FLW         0x01000000 /* front left wide */
#define LC3_CONFIG_CHNL_FRW         0x02000000 /* front right wide */
#define LC3_CONFIG_CHNL_LS          0x04000000 /* left surround */
#define LC3_CONFIG_CHNL_RS          0x08000000 /* right surround */

#define LC3_MAX_CHANNELS 28

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
        uint8_t  framing;
        uint8_t  phy;
        uint8_t  retransmission;
        uint16_t latency;
        uint32_t delay_min;
        uint32_t delay_max;
        uint32_t preferred_delay_min;
        uint32_t preferred_delay_max;
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

#endif
