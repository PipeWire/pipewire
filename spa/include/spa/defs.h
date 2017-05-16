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

#ifndef __SPA_DEFS_H__
#define __SPA_DEFS_H__

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

typedef enum {
  SPA_RESULT_ASYNC                     =  (1 << 30),
  SPA_RESULT_WAIT_SYNC                 =  2,
  SPA_RESULT_MODIFIED                  =  1,
  SPA_RESULT_OK                        =  0,
  SPA_RESULT_ERROR                     = -1,
  SPA_RESULT_ERRNO                     = -2,
  SPA_RESULT_INACTIVE                  = -3,
  SPA_RESULT_NO_FORMAT                 = -4,
  SPA_RESULT_INVALID_COMMAND           = -5,
  SPA_RESULT_INVALID_PORT              = -6,
  SPA_RESULT_HAVE_BUFFER               = -7,
  SPA_RESULT_NEED_BUFFER               = -8,
  SPA_RESULT_PORTS_CHANGED             = -9,
  SPA_RESULT_FORMAT_CHANGED            = -10,
  SPA_RESULT_PROPERTIES_CHANGED        = -11,
  SPA_RESULT_NOT_IMPLEMENTED           = -12,
  SPA_RESULT_INVALID_PROPERTY_INDEX    = -13,
  SPA_RESULT_PROPERTY_UNSET            = -14,
  SPA_RESULT_ENUM_END                  = -15,
  SPA_RESULT_WRONG_PROPERTY_TYPE       = -16,
  SPA_RESULT_WRONG_PROPERTY_SIZE       = -17,
  SPA_RESULT_INVALID_MEDIA_TYPE        = -18,
  SPA_RESULT_INVALID_FORMAT_PROPERTIES = -19,
  SPA_RESULT_FORMAT_INCOMPLETE         = -20,
  SPA_RESULT_INVALID_ARGUMENTS         = -21,
  SPA_RESULT_UNKNOWN_INTERFACE         = -22,
  SPA_RESULT_INVALID_DIRECTION         = -23,
  SPA_RESULT_TOO_MANY_PORTS            = -24,
  SPA_RESULT_INVALID_PROPERTY_ACCESS   = -25,
  SPA_RESULT_UNEXPECTED                = -26,
  SPA_RESULT_NO_BUFFERS                = -27,
  SPA_RESULT_INVALID_BUFFER_ID         = -28,
  SPA_RESULT_WRONG_STATE               = -29,
  SPA_RESULT_ASYNC_BUSY                = -30,
  SPA_RESULT_INVALID_OBJECT_ID         = -31,
  SPA_RESULT_NO_MEMORY                 = -32,
  SPA_RESULT_NO_PERMISSION             = -33,
  SPA_RESULT_SKIPPED                   = -34,
  SPA_RESULT_OUT_OF_BUFFERS            = -35,
  SPA_RESULT_INCOMPATIBLE_PROPS        = -36,
} SpaResult;

#define SPA_ASYNC_MASK                  (3 << 30)
#define SPA_ASYNC_SEQ_MASK              (SPA_RESULT_ASYNC - 1)

#define SPA_RESULT_IS_OK(res)           ((res) >= 0)
#define SPA_RESULT_IS_ERROR(res)        ((res) < 0)
#define SPA_RESULT_IS_ASYNC(res)        (((res) & SPA_ASYNC_MASK) == SPA_RESULT_ASYNC)

#define SPA_RESULT_ASYNC_SEQ(res)       ((res) & SPA_ASYNC_SEQ_MASK)
#define SPA_RESULT_RETURN_ASYNC(seq)    (SPA_RESULT_ASYNC | ((seq) & SPA_ASYNC_SEQ_MASK))

typedef enum {
  SPA_DIRECTION_INPUT  = 0,
  SPA_DIRECTION_OUTPUT = 1,
} SpaDirection;

typedef struct {
  uint32_t width;
  uint32_t height;
} SpaRectangle;

typedef struct {
  uint32_t num;
  uint32_t denom;
} SpaFraction;

typedef void (*SpaNotify) (void *data);

#define SPA_N_ELEMENTS(arr)  (sizeof (arr) / sizeof ((arr)[0]))
#define SPA_MIN(a,b)  ((a)<(b) ? (a) : (b))
#define SPA_MAX(a,b)  ((a)>(b) ? (a) : (b))
#define SPA_ABS(a)    ((a)>0 ? (a) : -(a))
#define SPA_CLAMP(v,a,b)  ((v)>(b) ? (b) : ((v) < (a) ? (a) : (v)))

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
# define SPA_PRINTF_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
# define SPA_ALIGNED(align) __attribute__ ((aligned (align)))
#else
# define SPA_PRINTF_FUNC(fmt, arg1)
# define SPA_ALIGNED(align)
#endif

#define SPA_ROUND_UP_N(num,align) ((((num) + ((align) - 1)) & ~((align) - 1)))

#ifndef SPA_LIKELY
#ifdef __GNUC__
#define SPA_LIKELY(x) (__builtin_expect(!!(x),1))
#define SPA_UNLIKELY(x) (__builtin_expect(!!(x),0))
#else
#define SPA_LIKELY(x) (x)
#define SPA_UNLIKELY(x) (x)
#endif
#endif

#define spa_return_if_fail(expr)                                        \
    do {                                                                \
        if (SPA_UNLIKELY (!(expr)))                                     \
            return;                                                     \
    } while(false)

#define spa_return_val_if_fail(expr, val)                               \
    do {                                                                \
        if (SPA_UNLIKELY(!(expr)))                                      \
            return (val);                                               \
    } while(false)

/* spa_assert_se() is an assert which guarantees side effects of x,
 * i.e. is never optimized away, regardless of NDEBUG or FASTPATH. */
#define spa_assert_se(expr)                                             \
    do {                                                                \
        if (SPA_UNLIKELY(!(expr)))                                      \
            abort();                                                    \
    } while (false)

/* Does exactly nothing */
#define spa_nop() do {} while (false)

#define spa_memzero(x,l) (memset((x), 0, (l)))
#define spa_zero(x) (spa_memzero(&(x), sizeof(x)))

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_DEFS_H__ */
