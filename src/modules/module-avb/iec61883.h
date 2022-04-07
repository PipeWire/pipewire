/* AVB support
 *
 * Copyright © 2022 Wim Taymans
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

#ifndef AVB_IEC61883_H
#define AVB_IEC61883_H

#include "packets.h"

struct avb_packet_iec61883 {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned mr:1;
	unsigned _r1:1;
	unsigned gv:1;
	unsigned tv:1;

	uint8_t seq_number;

	unsigned _r2:7;
	unsigned tu:1;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned tv:1;
	unsigned gv:1;
	unsigned _r1:1;
	unsigned mr:1;
	unsigned version:3;
	unsigned sv:1;

	uint8_t seq_num;

	unsigned tu:1;
	unsigned _r2:7;
#endif
	uint64_t stream_id;
	uint32_t timestamp;
	uint32_t gateway_info;
	uint16_t data_len;
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t tag:2;
	uint8_t channel:6;

	uint8_t tcode:4;
	uint8_t app:4;

	uint8_t qi1:2;		/* CIP Quadlet Indicator 1 */
	uint8_t sid:6;		/* CIP Source ID */

	uint8_t dbs;		/* CIP Data Block Size */

	uint8_t fn:2;		/* CIP Fraction Number */
	uint8_t qpc:3;		/* CIP Quadlet Padding Count */
	uint8_t sph:1;		/* CIP Source Packet Header */
	uint8_t _r3:2;

	uint8_t dbc;		/* CIP Data Block Continuity */

	uint8_t qi2:2;		/* CIP Quadlet Indicator 2 */
	uint8_t format_id:6;	/* CIP Format ID */
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t channel:6;
	uint8_t tag:2;

	uint8_t app:4;
	uint8_t tcode:4;

	uint8_t sid:6;		/* CIP Source ID */
	uint8_t qi1:2;		/* CIP Quadlet Indicator 1 */

	uint8_t dbs;		/* CIP Data Block Size */

	uint8_t _r3:2;
	uint8_t sph:1;		/* CIP Source Packet Header */
	uint8_t qpc:3;		/* CIP Quadlet Padding Count */
	uint8_t fn:2;		/* CIP Fraction Number */

	uint8_t dbc;		/* CIP Data Block Continuity */

	uint8_t format_id:6;	/* CIP Format ID */
	uint8_t qi2:2;		/* CIP Quadlet Indicator 2 */
#endif
	uint8_t fdf;		/* CIP Format Dependent Field */
        uint16_t syt;

	uint8_t payload[0];
} __attribute__ ((__packed__));

#endif /* AVB_IEC61883_H */
