/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_RTP_H
#define PIPEWIRE_RTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <spa/utils/defs.h>

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
	uint32_t csrc[];
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

static inline int16_t calculate_seqnum_delta(uint16_t seqnum_a, uint16_t seqnum_b)
{
	/* In RTP, sequence numbers are 16-bit unsigned integers. These
	 * can realistically reach the limit of that data type's range.
	 * In unsigned integer arithmetic, trying to increment past the
	 * range causes a wrap around. 65535 incremented by 1 becomes
	 * 0, for example.
	 *
	 * This matters when calculating deltas between sequence numbers.
	 * Straightforward cases like (500-450) are just like normal
	 * (the result would be 50 in this example). Same goes for
	 * reverse sequence numbers; 450-500 => -50. But when the first
	 * sequence number is something like 65535, and the second
	 * sequence number is something like 0, the straightforward
	 * method would result in something incorrect (a delta of
	 * -65535, when actually, the correct delta is 1).
	 *
	 * This code uses unsigned integer wrap-around arithmetic to
	 * handle such special sequence number wrap-around cases implicitly.
	 * A subtraction that normally would result in a negative value
	 * wraps around; subtracting 1 from 0, which normally would
	 * result in -1, becomes 65535 for example.
	 *
	 * By treating unsigned deltas below 32768 as positive deltas, and
	 * those >= 32768 as negative deltas, that arithmetic does the job
	 * for us. With two's complement, the cast directly reinterprets
	 * the bit pattern to produce the signed delta. (For example, uint16
	 * 65535 -> int16 -1). In the very rare case of a non-2-complement
	 * machine, an alternative  code is used that handles the >=32768
	 * unsigned delta case separately.
	 *
	 * This allows for positive sequence number deltas of up to 32767 and
	 * negative ones of up to -32768 to be handled correctly. Such vast
	 * deltas between sequence numbers are highly anomalous in RTP, so
	 * this covers all real world use cases well. */

#ifdef SPA_MACHINE_USES_TWOS_COMPLEMENT
	return (int16_t)(seqnum_b - seqnum_a);
#else
	uint16_t udelta = seqnum_b - seqnum_a;
	return (udelta < 32768) ? ((int32_t)udelta) : ((int32_t)udelta - 65536);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTP_H */
