/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_VBAN_H
#define PIPEWIRE_VBAN_H

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
	char vban[4];			/* contains 'V' 'B', 'A', 'N' */
	uint8_t format_SR;			/* SR index */
	uint8_t format_nbs;			/* nb sample per frame (1 to 256) */
	uint8_t format_nbc;			/* nb channel (1 to 256) */
	uint8_t format_bit;			/* bit format */
	char stream_name[VBAN_STREAM_NAME_SIZE];	/* stream name */
	uint32_t n_frames;			/* growing frame number. */
} __attribute__ ((packed));

#define VBAN_SR_MAXNUMBER	21

static uint32_t const vban_SR[VBAN_SR_MAXNUMBER] = {
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

#define VBAN_DATATYPE_U8	0x00
#define VBAN_DATATYPE_INT16	0x01
#define VBAN_DATATYPE_INT24	0x02
#define VBAN_DATATYPE_INT32	0x03
#define VBAN_DATATYPE_FLOAT32	0x04
#define VBAN_DATATYPE_FLOAT64	0x05
#define VBAN_DATATYPE_12BITS	0x06
#define VBAN_DATATYPE_10BITS	0x07

#define VBAN_SERIAL_GENERIC	0x00
#define VBAN_SERIAL_MIDI	0x10
#define VBAN_SERIAL_USER	0xf0

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_VBAN_H */
