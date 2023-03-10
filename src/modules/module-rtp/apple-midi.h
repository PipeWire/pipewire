/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_APPLE_MIDI_H
#define PIPEWIRE_APPLE_MIDI_H

#ifdef __cplusplus
extern "C" {
#endif

struct rtp_apple_midi {
	uint32_t cmd;
	uint32_t protocol;
	uint32_t initiator;
	uint32_t ssrc;
	char name[0];
} __attribute__ ((packed));

struct rtp_apple_midi_ck {
	uint32_t cmd;
	uint32_t ssrc;
	uint8_t count;
	uint8_t padding[3];
	uint32_t ts1_h;
	uint32_t ts1_l;
	uint32_t ts2_h;
	uint32_t ts2_l;
	uint32_t ts3_h;
	uint32_t ts3_l;
} __attribute__ ((packed));

struct rtp_apple_midi_rs {
	uint32_t cmd;
	uint32_t ssrc;
	uint32_t seqnum;
} __attribute__ ((packed));

#define APPLE_MIDI_CMD_IN	((0xffff << 16) | 'I'<<8 | 'N')
#define APPLE_MIDI_CMD_NO	((0xffff << 16) | 'N'<<8 | 'O')
#define APPLE_MIDI_CMD_OK	((0xffff << 16) | 'O'<<8 | 'K')
#define APPLE_MIDI_CMD_CK	((0xffff << 16) | 'C'<<8 | 'K')
#define APPLE_MIDI_CMD_BY	((0xffff << 16) | 'B'<<8 | 'Y')
#define APPLE_MIDI_CMD_RS	((0xffff << 16) | 'R'<<8 | 'S')

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_APPLE_MIDI_H */
