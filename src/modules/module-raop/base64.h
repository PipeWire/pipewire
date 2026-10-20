/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_BASE64_H
#define PIPEWIRE_BASE64_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void pw_base64_encode(const uint8_t *data, size_t len, char *enc, char pad)
{
	static const char tab[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;
	for (i = 0; i < len; i += 3) {
		uint32_t v;
		v  =              data[i+0]      << 16;
		v |= (i+1 < len ? data[i+1] : 0) << 8;
		v |= (i+2 < len ? data[i+2] : 0);
		*enc++ =             tab[(v >> (3*6)) & 0x3f];
		*enc++ =             tab[(v >> (2*6)) & 0x3f];
		*enc++ = i+1 < len ? tab[(v >> (1*6)) & 0x3f] : pad;
		*enc++ = i+2 < len ? tab[(v >> (0*6)) & 0x3f] : pad;
	}
	*enc = '\0';
}

static inline size_t pw_base64_decode(const char *data, size_t len, uint8_t *dec)
{
	static const int8_t tab[] = {
		62, -1, -1, -1, 63, 52, 53, 54, 55, 56,
		57, 58, 59, 60, 61, -1, -1, -1, -1, -1,
		-1, -1,  0,  1,  2,  3,  4,  5,  6,  7,
		 8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
		18, 19, 20, 21, 22, 23, 24, 25, -1, -1,
		-1, -1, -1, -1, 26, 27, 28, 29, 30, 31,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
		42, 43, 44, 45, 46, 47, 48, 49, 50, 51 };
	size_t i, j;
	/* Only decode whole 4-char quartets: a truncated tail cannot be decoded,
	 * and reading data[i+1..3] past it would be out of bounds. */
	for (i = 0, j = 0; i + 4 <= len; i += 4) {
		int8_t q[4];
		int k;
		uint32_t v;

		/* Map each byte through the alphabet, rejecting anything outside
		 * '+'(43)..'z'(122) or a hole in the table — an unchecked tab[c-43]
		 * would read outside the array. '=' is padding, decoded as 0. */
		for (k = 0; k < 4; k++) {
			unsigned char c = (unsigned char) data[i+k];
			if (c == '=')
				q[k] = 0;
			else if (c >= 43 && c <= 122 && tab[c - 43] >= 0)
				q[k] = tab[c - 43];
			else
				return j;	/* invalid input: stop cleanly */
		}

		v =  (uint32_t)q[0] << (3*6);
		v |= (uint32_t)q[1] << (2*6);
		v |= (uint32_t)q[2] << (1*6);
		v |= (uint32_t)q[3];
		                      dec[j++] = (v >> 16) & 0xff;
		if (data[i+2] != '=') dec[j++] = (v >> 8)  & 0xff;
		if (data[i+3] != '=') dec[j++] =  v        & 0xff;
	}
	return j;
}


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_BASE64_H */
