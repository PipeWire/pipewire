/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
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

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTP_H */
