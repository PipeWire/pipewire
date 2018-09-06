/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_UTILS_DEFS_H__
#define __SPA_UTILS_DEFS_H__

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>


#define SPA_ASYNC_BIT			(1 << 30)
#define SPA_ASYNC_MASK			(3 << 30)
#define SPA_ASYNC_SEQ_MASK		(SPA_ASYNC_BIT - 1)

#define SPA_RESULT_IS_OK(res)		((res) >= 0)
#define SPA_RESULT_IS_ERROR(res)	((res) < 0)
#define SPA_RESULT_IS_ASYNC(res)	(((res) & SPA_ASYNC_MASK) == SPA_ASYNC_BIT)

#define SPA_RESULT_ASYNC_SEQ(res)	((res) & SPA_ASYNC_SEQ_MASK)
#define SPA_RESULT_RETURN_ASYNC(seq)	(SPA_ASYNC_BIT | ((seq) & SPA_ASYNC_SEQ_MASK))

#define SPA_FLAG_CHECK(field,flag)	(((field) & (flag)) == (flag))
#define SPA_FLAG_SET(field,flag)	((field) |= (flag))
#define SPA_FLAG_UNSET(field,flag)	((field) &= ~(flag))

enum spa_direction {
	SPA_DIRECTION_INPUT = 0,
	SPA_DIRECTION_OUTPUT = 1,
};

#define SPA_RECTANGLE(width,height) (struct spa_rectangle){ width, height }

struct spa_rectangle {
	uint32_t width;
	uint32_t height;
};

#define SPA_FRACTION(num,denom) (struct spa_fraction){ num, denom }
struct spa_fraction {
	uint32_t num;
	uint32_t denom;
};

#define SPA_N_ELEMENTS(arr)  (sizeof(arr) / sizeof((arr)[0]))

#define SPA_MIN(a,b)		\
({				\
	__typeof__(a) _a = (a);	\
	__typeof__(b) _b = (b);	\
	_a < _b ? _a : _b;	\
})
#define SPA_MAX(a,b)		\
({				\
	__typeof__(a) _a = (a);	\
	__typeof__(b) _b = (b);	\
	_a > _b ? _a : _b;	\
})
#define SPA_CLAMP(v,low,high)				\
({							\
	__typeof__(v) _v = (v);				\
	__typeof__(low) _low = (low);			\
	__typeof__(high) _high = (high);		\
	_v > _high ? _high : ( _v < _low ? _low : _v);	\
})

#define SPA_MEMBER(b,o,t) ((t*)((uint8_t*)(b) + (o)))

#define SPA_CONTAINER_OF(p,t,m) (t*)((uint8_t*)p - offsetof (t,m))

#define SPA_PTRDIFF(p1,p2) ((uint8_t*)(p1) - (uint8_t*)(p2))

#define SPA_PTR_TO_INT(p) ((int) ((intptr_t) (p)))
#define SPA_INT_TO_PTR(u) ((void*) ((intptr_t) (u)))

#define SPA_PTR_TO_UINT32(p) ((uint32_t) ((uintptr_t) (p)))
#define SPA_UINT32_TO_PTR(u) ((void*) ((uintptr_t) (u)))

#define SPA_TIME_INVALID  ((int64_t)INT64_MIN)
#define SPA_IDX_INVALID  ((unsigned int)-1)
#define SPA_ID_INVALID  ((uint32_t)0xffffffff)

#define SPA_NSEC_PER_SEC  (1000000000ll)
#define SPA_NSEC_PER_MSEC (1000000ll)
#define SPA_NSEC_PER_USEC (1000ll)
#define SPA_USEC_PER_SEC  (1000000ll)
#define SPA_USEC_PER_MSEC (1000ll)
#define SPA_MSEC_PER_SEC  (1000ll)

#define SPA_TIMESPEC_TO_TIME(ts) ((ts)->tv_sec * SPA_NSEC_PER_SEC + (ts)->tv_nsec)
#define SPA_TIMEVAL_TO_TIME(tv)  ((tv)->tv_sec * SPA_NSEC_PER_SEC + (tv)->tv_usec * 1000ll)

#ifdef __GNUC__
#define SPA_PRINTF_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#define SPA_ALIGNED(align) __attribute__((aligned(align)))
#define SPA_DEPRECATED __attribute__ ((deprecated))
#else
#define SPA_PRINTF_FUNC(fmt, arg1)
#define SPA_ALIGNED(align)
#define SPA_DEPRECATED
#endif

#define SPA_ROUND_DOWN_N(num,align)	((num) & ~((align) - 1))
#define SPA_ROUND_UP_N(num,align)	SPA_ROUND_DOWN_N((num) + ((align) - 1),align)

#ifndef SPA_LIKELY
#ifdef __GNUC__
#define SPA_LIKELY(x) (__builtin_expect(!!(x),1))
#define SPA_UNLIKELY(x) (__builtin_expect(!!(x),0))
#else
#define SPA_LIKELY(x) (x)
#define SPA_UNLIKELY(x) (x)
#endif
#endif

#define SPA_STRINGIFY_1(x...)	#x
#define SPA_STRINGIFY(x...)	SPA_STRINGIFY_1(x)

#define spa_return_if_fail(expr)					\
	do {								\
		if (SPA_UNLIKELY(!(expr)))				\
			return;						\
	} while(false)

#define spa_return_val_if_fail(expr, val)				\
	do {								\
		if (SPA_UNLIKELY(!(expr)))				\
			return (val);					\
	} while(false)

/* spa_assert_se() is an assert which guarantees side effects of x,
 * i.e. is never optimized away, regardless of NDEBUG or FASTPATH. */
#define spa_assert_se(expr)						\
	do {								\
		if (SPA_UNLIKELY(!(expr)))				\
			abort();					\
	} while (false)

/* Does exactly nothing */
#define spa_nop() do {} while (false)

#define spa_memzero(x,l) (memset((x), 0, (l)))
#define spa_zero(x) (spa_memzero(&(x), sizeof(x)))

#define spa_strerror(err)		\
({					\
	int __err = -err;		\
	if (SPA_RESULT_IS_ASYNC(err))	\
		__err = EINPROGRESS;	\
	strerror(__err);		\
})

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_UTILS_DEFS_H__ */
