/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_VBAN_H
#define PIPEWIRE_VBAN_H

#include <stdint.h>

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VBAN_HEADER_SIZE	(4 + 4 + 16 + 4)
#define VBAN_STREAM_NAME_SIZE	16
#define VBAN_PROTOCOL_MAX_SIZE	1464
#define VBAN_DATA_MAX_SIZE	(VBAN_PROTOCOL_MAX_SIZE - VBAN_HEADER_SIZE)
#define VBAN_CHANNELS_MAX_NB	256
#define VBAN_SAMPLES_MAX_NB	256

struct vban_header {
	char vban[4];					/* contains 'V' 'B', 'A', 'N' */
	uint8_t format_SR;				/* SR index */
	uint8_t format_nbs;				/* nb sample per frame (1 to 256) */
	uint8_t format_nbc;				/* nb channel (1 to 256) */
	uint8_t format_bit;				/* bit format */
	char stream_name[VBAN_STREAM_NAME_SIZE];	/* stream name */
	uint32_t n_frames;				/* growing frame number. */
} __attribute__ ((packed));

#define VBAN_PROTOCOL_AUDIO		0x00
#define VBAN_PROTOCOL_SERIAL		0x20
#define VBAN_PROTOCOL_TXT		0x40
#define VBAN_PROTOCOL_SERVICE		0x60
#define VBAN_PROTOCOL_UNDEFINED_1	0x80
#define VBAN_PROTOCOL_UNDEFINED_2	0xA0
#define VBAN_PROTOCOL_UNDEFINED_3	0xC0
#define VBAN_PROTOCOL_USER		0xE0

#define VBAN_SR_MAXNUMBER	21

static uint32_t const vban_SR[32] = {
	6000, 12000, 24000, 48000, 96000, 192000, 384000,
	8000, 16000, 32000, 64000, 128000, 256000, 512000,
	11025, 22050, 44100, 88200, 176400, 352800, 705600
};

static inline uint8_t vban_sr_index(uint32_t rate)
{
	uint8_t i;
	for (i = 0; i < SPA_N_ELEMENTS(vban_SR); i++) {
		if (vban_SR[i] == rate)
			return i;
	}
	return VBAN_SR_MAXNUMBER;
}

#define VBAN_CODEC_PCM		0x00
#define VBAN_CODEC_VBCA		0x10 //VB-AUDIO AOIP CODEC
#define VBAN_CODEC_VBCV		0x20 //VB-AUDIO VOIP CODEC
#define VBAN_CODEC_UNDEFINED_1	0x30
#define VBAN_CODEC_UNDEFINED_2	0x40
#define VBAN_CODEC_UNDEFINED_3	0x50
#define VBAN_CODEC_UNDEFINED_4	0x60
#define VBAN_CODEC_UNDEFINED_5	0x70
#define VBAN_CODEC_UNDEFINED_6	0x80
#define VBAN_CODEC_UNDEFINED_7	0x90
#define VBAN_CODEC_UNDEFINED_8	0xA0
#define VBAN_CODEC_UNDEFINED_9	0xB0
#define VBAN_CODEC_UNDEFINED_10	0xC0
#define VBAN_CODEC_UNDEFINED_11	0xD0
#define VBAN_CODEC_UNDEFINED_12	0xE0
#define VBAN_CODEC_USER		0xF0

#define VBAN_BPS_0		0
#define VBAN_BPS_110		1
#define VBAN_BPS_150		2
#define VBAN_BPS_300		3
#define VBAN_BPS_600		4
#define VBAN_BPS_1200		5
#define VBAN_BPS_2400		6
#define VBAN_BPS_4800		7
#define VBAN_BPS_9600		8
#define VBAN_BPS_14400		9
#define VBAN_BPS_19200		10
#define VBAN_BPS_31250		11
#define VBAN_BPS_38400		12
#define VBAN_BPS_57600		13
#define VBAN_BPS_115200		14
#define VBAN_BPS_128000		15
#define VBAN_BPS_230400		16
#define VBAN_BPS_250000		17
#define VBAN_BPS_256000		18
#define VBAN_BPS_460800		19
#define VBAN_BPS_921600		20
#define VBAN_BPS_1000000	21
#define VBAN_BPS_1500000	22
#define VBAN_BPS_2000000	23
#define VBAN_BPS_3000000	24
#define VBAN_BPS_MAXNUMBER	25

static const int vban_BPSList[] = {
	[VBAN_BPS_0] = 0,
	[VBAN_BPS_110] = 110,
	[VBAN_BPS_150] = 150,
	[VBAN_BPS_300] = 300,
	[VBAN_BPS_600] = 600,
	[VBAN_BPS_1200] = 1200,
	[VBAN_BPS_2400] = 2400,
	[VBAN_BPS_4800] = 4800,
	[VBAN_BPS_9600] = 9600,
	[VBAN_BPS_14400] = 14400,
	[VBAN_BPS_19200] = 19200,
	[VBAN_BPS_31250] = 31250,
	[VBAN_BPS_38400] = 38400,
	[VBAN_BPS_57600] = 57600,
	[VBAN_BPS_115200] = 115200,
	[VBAN_BPS_128000] = 128000,
	[VBAN_BPS_230400] = 230400,
	[VBAN_BPS_250000] = 250000,
	[VBAN_BPS_256000] = 256000,
	[VBAN_BPS_460800] = 460800,
	[VBAN_BPS_921600] = 921600,
	[VBAN_BPS_1000000] = 1000000,
	[VBAN_BPS_1500000] = 1500000,
	[VBAN_BPS_2000000] = 2000000,
	[VBAN_BPS_3000000] = 3000000,
};
SPA_STATIC_ASSERT(SPA_N_ELEMENTS(vban_BPSList) == VBAN_BPS_MAXNUMBER);

#define VBAN_SERIAL_GENERIC	0x00
#define VBAN_SERIAL_MIDI	0x10
#define VBAN_SERIAL_USER	0xf0

#define VBAN_DATATYPE_BYTE8	0x00
#define VBAN_DATATYPE_INT16	0x01
#define VBAN_DATATYPE_INT24	0x02
#define VBAN_DATATYPE_INT32	0x03
#define VBAN_DATATYPE_FLOAT32	0x04
#define VBAN_DATATYPE_FLOAT64	0x05
#define VBAN_DATATYPE_12BITS	0x06
#define VBAN_DATATYPE_10BITS	0x07

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_VBAN_H */
