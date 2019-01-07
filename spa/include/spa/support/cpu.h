/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
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

#ifndef __SPA_CPU_H__
#define __SPA_CPU_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/utils/defs.h>

#define SPA_CPU_FLAG_MMX	(1<<0)	/**< standard MMX */
#define SPA_CPU_FLAG_MMXEXT	(1<<1)	/**< SSE integer or AMD MMX ext */
#define SPA_CPU_FLAG_3DNOW	(1<<2)	/**< AMD 3DNOW */
#define SPA_CPU_FLAG_SSE	(1<<3)	/**< SSE */
#define SPA_CPU_FLAG_SSE2	(1<<4)	/**< SSE2 */
#define SPA_CPU_FLAG_3DNOWEXT	(1<<5)	/**< AMD 3DNowExt */
#define SPA_CPU_FLAG_SSE3	(1<<6)	/**< Prescott SSE3 */
#define SPA_CPU_FLAG_SSSE3	(1<<7)	/**< Conroe SSSE3 */
#define SPA_CPU_FLAG_SSE41	(1<<8)	/**< Penryn SSE4.1 */
#define SPA_CPU_FLAG_SSE42	(1<<9)	/**< Nehalem SSE4.2 */
#define SPA_CPU_FLAG_AESNI	(1<<10)	/**< Advanced Encryption Standard */
#define SPA_CPU_FLAG_AVX	(1<<11)	/**< AVX */
#define SPA_CPU_FLAG_XOP	(1<<12)	/**< Bulldozer XOP */
#define SPA_CPU_FLAG_FMA4	(1<<13)	/**< Bulldozer FMA4 */
#define SPA_CPU_FLAG_CMOV	(1<<14)	/**< supports cmov */
#define SPA_CPU_FLAG_AVX2	(1<<15)	/**< AVX2 */
#define SPA_CPU_FLAG_FMA3	(1<<16)	/**< Haswell FMA3 */
#define SPA_CPU_FLAG_BMI1	(1<<17)	/**< Bit Manipulation Instruction Set 1 */
#define SPA_CPU_FLAG_BMI2	(1<<18)	/**< Bit Manipulation Instruction Set 2 */
#define SPA_CPU_FLAG_AVX512	(1<<19)	/**< AVX-512 */

#define SPA_CPU_FLAG_ALTIVEC	(1<<0)	/**< standard */
#define SPA_CPU_FLAG_VSX	(1<<1)	/**< ISA 2.06 */
#define SPA_CPU_FLAG_POWER8	(1<<2)	/**< ISA 2.07 */

#define SPA_CPU_FLAG_ARMV5TE	(1 << 0)
#define SPA_CPU_FLAG_ARMV6	(1 << 1)
#define SPA_CPU_FLAG_ARMV6T2	(1 << 2)
#define SPA_CPU_FLAG_VFP	(1 << 3)
#define SPA_CPU_FLAG_VFPV3	(1 << 4)
#define SPA_CPU_FLAG_NEON	(1 << 5)
#define SPA_CPU_FLAG_ARMV8	(1 << 6)

#define SPA_CPU_FORCE_AUTODETECT	((uint32_t)-1)
/**
 * The CPU features interface
 */
struct spa_cpu {
	/** the version of this interface. This can be used to expand this
	  structure in the future */
#define SPA_VERSION_CPU	0
	uint32_t version;
	/**
	 * Extra information about the interface
	 */
	const struct spa_dict *info;

	/** get CPU flags */
	uint32_t (*get_flags) (struct spa_cpu *cpu);

	/** force CPU flags, use SPA_CPU_FORCE_AUTODETECT to autodetect CPU flags */
	int (*force_flags) (struct spa_cpu *cpu, uint32_t flags);

	/** get number of CPU cores */
	uint32_t (*get_count) (struct spa_cpu *cpu);

	/** get maximum required alignment of data */
	uint32_t (*get_max_align) (struct spa_cpu *cpu);
};

#define spa_cpu_get_flags(c)		(c)->get_flags((c))
#define spa_cpu_force_flags(c,f)	(c)->force_flags((c), (f))
#define spa_cpu_get_count(c)		(c)->get_count((c))
#define spa_cpu_get_max_align(c)	(c)->get_max_align((c))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_CPU_H__ */
