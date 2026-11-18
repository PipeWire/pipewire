/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_ENDIAN_H
#define SPA_ENDIAN_H

#if defined(__MidnightBSD__)
#include <sys/endian.h>
#define bswap_16 bswap16
#define bswap_32 bswap32
#define bswap_64 bswap64
#elif defined(_MSC_VER) && defined(_WIN32)
#include <stdlib.h>
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#define bswap_16 _byteswap_ushort
#define bswap_32 _byteswap_ulong
#define bswap_64 _byteswap_uint64
#else
#include <endian.h>
#ifdef __has_include
#if __has_include(<byteswap.h>)
#include <byteswap.h>
#endif
#endif
#ifndef bswap_16
#define bswap_16 __builtin_bswap16
#define bswap_32 __builtin_bswap32
#define bswap_64 __builtin_bswap64
#endif
#ifndef __BYTE_ORDER
#define __LITTLE_ENDIAN	__ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN	__ORDER_BIG_ENDIAN__
#define __BYTE_ORDER	__BYTE_ORDER__
#endif
#endif

#endif /* SPA_ENDIAN_H */
