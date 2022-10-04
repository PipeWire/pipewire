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

#ifndef PIPEWIRE_SAP_H
#define PIPEWIRE_SAP_H

#ifdef __cplusplus
extern "C" {
#endif

struct sap_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned c:1;
	unsigned e:1;
	unsigned t:1;
	unsigned r:1;
	unsigned a:1;
	unsigned v:3;
#elif __BYTE_ORDER == __BIG_ENDIAN
	unsigned v:3;
	unsigned a:1;
	unsigned r:1;
	unsigned t:1;
	unsigned e:1;
	unsigned c:1;
#else
#error "Unknown byte order"
#endif
	uint8_t auth_len;
	uint16_t msg_id_hash;
} __attribute__ ((packed));

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_SAP_H */
