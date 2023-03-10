/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_RTP_H
#define PIPEWIRE_RTP_H

#ifdef __cplusplus
extern "C" {
#endif

struct rtp_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned cc:4;
	unsigned x:1;
	unsigned p:1;
	unsigned v:2;

	unsigned pt:7;
	unsigned m:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	unsigned v:2;
	unsigned p:1;
	unsigned x:1;
	unsigned cc:4;

	unsigned m:1;
	unsigned pt:7;
#else
#error "Unknown byte order"
#endif
	uint16_t sequence_number;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t csrc[0];
} __attribute__ ((packed));

struct rtp_payload {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned frame_count:4;
	unsigned rfa0:1;
	unsigned is_last_fragment:1;
	unsigned is_first_fragment:1;
	unsigned is_fragmented:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	unsigned is_fragmented:1;
	unsigned is_first_fragment:1;
	unsigned is_last_fragment:1;
	unsigned rfa0:1;
	unsigned frame_count:4;
#endif
} __attribute__ ((packed));

struct rtp_midi_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned len:4;
	unsigned p:1;
	unsigned z:1;
	unsigned j:1;
	unsigned b:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	unsigned b:1;
	unsigned j:1;
	unsigned z:1;
	unsigned p:1;
	unsigned len:4;
#endif
	uint8_t len_b;
} __attribute__ ((packed));

struct rtp_midi_journal {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned totchan:4;
	unsigned H:1;
	unsigned A:1;
	unsigned Y:1;
	unsigned S:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
	unsigned S:1;
	unsigned Y:1;
	unsigned A:1;
	unsigned H:1;
	unsigned totchan:4;
#endif
	uint16_t checkpoint_seqnum;
} __attribute__ ((packed));


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTP_H */
