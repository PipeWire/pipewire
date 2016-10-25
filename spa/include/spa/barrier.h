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

#ifndef __SPA_BARRIER_H__
#define __SPA_BARRIER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>

#if defined(__GNUC__)
#  if defined( __i386__ ) || defined( __i486__ ) || defined( __i586__ ) || defined( __i686__ ) || defined( __x86_64__ )
#    define spa_barrier_full()  asm volatile("mfence":::"memory")
#    define spa_barrier_read()  asm volatile("lfence":::"memory")
#    define spa_barrier_write() asm volatile("sfence":::"memory")
#  elif (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
#    define spa_barrier_full()  __sync_synchronize()
#    define spa_barrier_read()  __sync_synchronize()
#    define spa_barrier_write() __sync_synchronize()
#  endif
#else
#  warning no memory barriers support found
#  define spa_barrier_full()
#  define spa_barrier_read()
#  define spa_barrier_write()
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_BARRIER_H__ */
