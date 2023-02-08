/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

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
