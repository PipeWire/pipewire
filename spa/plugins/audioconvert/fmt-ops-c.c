/* Spa
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

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "fmt-ops.h"
#include "law.h"

#define MAKE_COPY(size)	 							\
void conv_copy ##size## d_c(struct convert *conv,				\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
		uint32_t n_samples)						\
{										\
	uint32_t i, n_channels = conv->n_channels;				\
	for (i = 0; i < n_channels; i++)					\
		spa_memcpy(dst[i], src[i], n_samples * (size>>3));		\
}										\
void conv_copy ##size## _c(struct convert *conv,				\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
		uint32_t n_samples)						\
{										\
	spa_memcpy(dst[0], src[0], n_samples * conv->n_channels * (size>>3));	\
}

MAKE_COPY(8);
MAKE_COPY(16);
MAKE_COPY(24);
MAKE_COPY(32);
MAKE_COPY(64);

#define MAKE_D_TO_D(sname,stype,dname,dtype,func) 				\
void conv_ ##sname## d_to_ ##dname## d_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (i = 0; i < n_channels; i++) {					\
		const stype *s = src[i];					\
		dtype *d = dst[i];						\
		for (j = 0; j < n_samples; j++)					\
			d[j] = func (s[j]);			 		\
	}									\
}

#define MAKE_I_TO_I(sname,stype,dname,dtype,func) 				\
void conv_ ##sname## _to_ ##dname## _c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t j;								\
	const stype *s = src[0];						\
	dtype *d = dst[0];							\
	n_samples *= conv->n_channels;						\
	for (j = 0; j < n_samples; j++)						\
		d[j] = func (s[j]);				 		\
}

#define MAKE_I_TO_D(sname,stype,dname,dtype,func) 				\
void conv_ ##sname## _to_ ##dname## d_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const stype *s = src[0];						\
	dtype **d = (dtype**)dst;						\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (j = 0; j < n_samples; j++) {					\
		for (i = 0; i < n_channels; i++)				\
			d[i][j] = func (*s++);			 		\
	}									\
}

#define MAKE_D_TO_I(sname,stype,dname,dtype,func) 				\
void conv_ ##sname## d_to_ ##dname## _c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const stype **s = (const stype **)src;					\
	dtype *d = dst[0];							\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (j = 0; j < n_samples; j++) {					\
		for (i = 0; i < n_channels; i++)				\
			*d++ = func (s[i][j]);			 		\
	}									\
}

/* to f32 */
MAKE_D_TO_D(u8, uint8_t, f32, float, U8_TO_F32);
MAKE_I_TO_I(u8, uint8_t, f32, float, U8_TO_F32);
MAKE_I_TO_D(u8, uint8_t, f32, float, U8_TO_F32);
MAKE_D_TO_I(u8, uint8_t, f32, float, U8_TO_F32);

MAKE_D_TO_D(s8, int8_t, f32, float, S8_TO_F32);
MAKE_I_TO_I(s8, int8_t, f32, float, S8_TO_F32);
MAKE_I_TO_D(s8, int8_t, f32, float, S8_TO_F32);
MAKE_D_TO_I(s8, int8_t, f32, float, S8_TO_F32);

MAKE_I_TO_D(alaw, uint8_t, f32, float, alaw_to_f32);
MAKE_I_TO_D(ulaw, uint8_t, f32, float, ulaw_to_f32);

MAKE_I_TO_I(u16, uint16_t, f32, float, U16_TO_F32);
MAKE_I_TO_D(u16, uint16_t, f32, float, U16_TO_F32);

MAKE_D_TO_D(s16, int16_t, f32, float, S16_TO_F32);
MAKE_I_TO_I(s16, int16_t, f32, float, S16_TO_F32);
MAKE_I_TO_D(s16, int16_t, f32, float, S16_TO_F32);
MAKE_D_TO_I(s16, int16_t, f32, float, S16_TO_F32);
MAKE_I_TO_D(s16s, uint16_t, f32, float, S16S_TO_F32);

MAKE_I_TO_I(u32, uint32_t, f32, float, U32_TO_F32);
MAKE_I_TO_D(u32, uint32_t, f32, float, U32_TO_F32);

MAKE_D_TO_D(s32, int32_t, f32, float, S32_TO_F32);
MAKE_I_TO_I(s32, int32_t, f32, float, S32_TO_F32);
MAKE_I_TO_D(s32, int32_t, f32, float, S32_TO_F32);
MAKE_D_TO_I(s32, int32_t, f32, float, S32_TO_F32);
MAKE_I_TO_D(s32s, uint32_t, f32, float, S32S_TO_F32);

MAKE_I_TO_I(u24, uint24_t, f32, float, U24_TO_F32);
MAKE_I_TO_D(u24, uint24_t, f32, float, U24_TO_F32);

MAKE_D_TO_D(s24, int24_t, f32, float, S24_TO_F32);
MAKE_I_TO_I(s24, int24_t, f32, float, S24_TO_F32);
MAKE_I_TO_D(s24, int24_t, f32, float, S24_TO_F32);
MAKE_D_TO_I(s24, int24_t, f32, float, S24_TO_F32);
MAKE_I_TO_D(s24s, int24_t, f32, float, S24S_TO_F32);

MAKE_I_TO_I(u24_32, uint32_t, f32, float, U24_32_TO_F32);
MAKE_I_TO_D(u24_32, uint32_t, f32, float, U24_32_TO_F32);

MAKE_D_TO_D(s24_32, int32_t, f32, float, S24_32_TO_F32);
MAKE_I_TO_I(s24_32, int32_t, f32, float, S24_32_TO_F32);
MAKE_I_TO_D(s24_32, int32_t, f32, float, S24_32_TO_F32);
MAKE_D_TO_I(s24_32, int32_t, f32, float, S24_32_TO_F32);
MAKE_I_TO_D(s24_32s, uint32_t, f32, float, S24_32S_TO_F32);

MAKE_D_TO_D(f64, double, f32, float, (float));
MAKE_I_TO_I(f64, double, f32, float, (float));
MAKE_I_TO_D(f64, double, f32, float, (float));
MAKE_D_TO_I(f64, double, f32, float, (float));
MAKE_I_TO_D(f64s, double, f32, float, bswap_64); /* FIXME */


/* from f32 */
MAKE_D_TO_D(f32, float, u8, uint8_t, F32_TO_U8);
MAKE_I_TO_I(f32, float, u8, uint8_t, F32_TO_U8);
MAKE_I_TO_D(f32, float, u8, uint8_t, F32_TO_U8);
MAKE_D_TO_I(f32, float, u8, uint8_t, F32_TO_U8);

MAKE_D_TO_D(f32, float, s8, int8_t, F32_TO_S8);
MAKE_I_TO_I(f32, float, s8, int8_t, F32_TO_S8);
MAKE_I_TO_D(f32, float, s8, int8_t, F32_TO_S8);
MAKE_D_TO_I(f32, float, s8, int8_t, F32_TO_S8);

MAKE_D_TO_I(f32, float, alaw, uint8_t, f32_to_alaw);
MAKE_D_TO_I(f32, float, ulaw, uint8_t, f32_to_ulaw);

MAKE_I_TO_I(f32, float, u16, uint16_t, F32_TO_U16);
MAKE_D_TO_I(f32, float, u16, uint16_t, F32_TO_U16);

MAKE_D_TO_D(f32, float, s16, int16_t, F32_TO_S16);
MAKE_I_TO_I(f32, float, s16, int16_t, F32_TO_S16);
MAKE_I_TO_D(f32, float, s16, int16_t, F32_TO_S16);
MAKE_D_TO_I(f32, float, s16, int16_t, F32_TO_S16);
MAKE_D_TO_I(f32, float, s16s, uint16_t, F32_TO_S16S);

MAKE_I_TO_I(f32, float, u32, uint32_t, F32_TO_U32);
MAKE_D_TO_I(f32, float, u32, uint32_t, F32_TO_U32);

MAKE_D_TO_D(f32, float, s32, int32_t, F32_TO_S32);
MAKE_I_TO_I(f32, float, s32, int32_t, F32_TO_S32);
MAKE_I_TO_D(f32, float, s32, int32_t, F32_TO_S32);
MAKE_D_TO_I(f32, float, s32, int32_t, F32_TO_S32);
MAKE_D_TO_I(f32, float, s32s, uint32_t, F32_TO_S32S);

MAKE_I_TO_I(f32, float, u24, uint24_t, F32_TO_U24);
MAKE_D_TO_I(f32, float, u24, uint24_t, F32_TO_U24);

MAKE_D_TO_D(f32, float, s24, int24_t, F32_TO_S24);
MAKE_I_TO_I(f32, float, s24, int24_t, F32_TO_S24);
MAKE_I_TO_D(f32, float, s24, int24_t, F32_TO_S24);
MAKE_D_TO_I(f32, float, s24, int24_t, F32_TO_S24);
MAKE_D_TO_I(f32, float, s24s, int24_t, F32_TO_S24S);

MAKE_I_TO_I(f32, float, u24_32, uint32_t, F32_TO_U24_32);
MAKE_D_TO_I(f32, float, u24_32, uint32_t, F32_TO_U24_32);

MAKE_D_TO_D(f32, float, s24_32, int32_t, F32_TO_S24_32);
MAKE_I_TO_I(f32, float, s24_32, int32_t, F32_TO_S24_32);
MAKE_I_TO_D(f32, float, s24_32, int32_t, F32_TO_S24_32);
MAKE_D_TO_I(f32, float, s24_32, int32_t, F32_TO_S24_32);
MAKE_D_TO_I(f32, float, s24_32s, uint32_t, F32_TO_S24_32S);

MAKE_D_TO_D(f32, float, f64, double, (double));
MAKE_I_TO_I(f32, float, f64, double, (double));
MAKE_I_TO_D(f32, float, f64, double, (double));
MAKE_D_TO_I(f32, float, f64, double, (double));
MAKE_D_TO_I(f32, float, f64s, double, bswap_32); /* FIXME */


static inline int32_t
lcnoise(uint32_t *state)
{
        *state = (*state * 96314165) + 907633515;
        return (int32_t)(*state);
}

static inline void update_dither_c(struct convert *conv, uint32_t n_samples)
{
	uint32_t n;
	float *dither = conv->dither, scale = conv->scale;
	uint32_t *state = &conv->random[0];

	for (n = 0; n < n_samples; n++)
		dither[n] = lcnoise(state) * scale;
}

#define MAKE_D_dither(dname,dtype,func) 					\
void conv_f32d_to_ ##dname## d_dither_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;	\
	float *dither = conv->dither;						\
	update_dither_c(conv, SPA_MIN(n_samples, dither_size));			\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = dst[i];						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, dither_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j] = func (s[j], dither[k]);			\
		}								\
	}									\
}

#define MAKE_I_dither(dname,dtype,func) 					\
void conv_f32d_to_ ##dname## _dither_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const float **s = (const float **) src;					\
	dtype *d = dst[0];							\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;	\
	float *dither = conv->dither;						\
	update_dither_c(conv, SPA_MIN(n_samples, dither_size));			\
	for (j = 0; j < n_samples;) {						\
		chunk = SPA_MIN(n_samples - j, dither_size);			\
		for (k = 0; k < chunk; k++, j++) {				\
			for (i = 0; i < n_channels; i++)			\
				*d++ = func (s[i][j], dither[k]);		\
		}								\
	}									\
}

MAKE_D_dither(u8, uint8_t, F32_TO_U8_D);
MAKE_I_dither(u8, uint8_t, F32_TO_U8_D);
MAKE_D_dither(s8, int8_t, F32_TO_S8_D);
MAKE_I_dither(s8, int8_t, F32_TO_S8_D);
MAKE_D_dither(s16, int16_t, F32_TO_S16_D);
MAKE_I_dither(s16, int16_t, F32_TO_S16_D);
MAKE_I_dither(s16s, uint16_t, F32_TO_S16S_D);
MAKE_D_dither(s32, int32_t, F32_TO_S32_D);
MAKE_I_dither(s32, int32_t, F32_TO_S32_D);
MAKE_I_dither(s32s, uint32_t, F32_TO_S32S_D);
MAKE_D_dither(s24, int24_t, F32_TO_S24_D);
MAKE_I_dither(s24, int24_t, F32_TO_S24_D);
MAKE_I_dither(s24s, int24_t, F32_TO_S24_D);
MAKE_D_dither(s24_32, int32_t, F32_TO_S24_32_D);
MAKE_I_dither(s24_32, int32_t, F32_TO_S24_32_D);
MAKE_I_dither(s24_32s, int32_t, F32_TO_S24_32S_D);


#define SHAPER5(type,s,scale,offs,sh,min,max,d)			\
({								\
	type t;							\
	float v = s * scale + offs +				\
		- sh->e[idx] * 2.033f				\
		+ sh->e[(idx - 1) & NS_MASK] * 2.165f		\
		- sh->e[(idx - 2) & NS_MASK] * 1.959f		\
		+ sh->e[(idx - 3) & NS_MASK] * 1.590f		\
		- sh->e[(idx - 4) & NS_MASK] * 0.6149f;		\
	t = (type)SPA_CLAMP(v + d, min, max);			\
	idx = (idx + 1) & NS_MASK;				\
	sh->e[idx] = t - v;					\
	t;							\
})

#define F32_TO_U8_SH(s,sh,d)	SHAPER5(uint8_t, s, U8_SCALE, U8_OFFS, sh, U8_MIN, U8_MAX, d)
#define F32_TO_S8_SH(s,sh,d)	SHAPER5(int8_t, s, S8_SCALE, 0, sh, S8_MIN, S8_MAX, d)
#define F32_TO_S16_SH(s,sh,d)	SHAPER5(int16_t, s, S16_SCALE, 0, sh, S16_MIN, S16_MAX, d)
#define F32_TO_S16S_SH(s,sh,d)	bswap_16(F32_TO_S16_SH(s,sh,d))

#define MAKE_D_shaped(dname,dtype,func) 					\
void conv_f32d_to_ ##dname## d_shaped_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;	\
	float *dither = conv->dither;						\
	update_dither_c(conv, SPA_MIN(n_samples, dither_size));			\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = dst[i];						\
		struct shaper *sh = &conv->shaper[i];				\
		uint32_t idx = sh->idx;						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, dither_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j] = func (s[j], sh, dither[k]);		\
		}								\
		sh->idx = idx;							\
	}									\
}

#define MAKE_I_shaped(dname,dtype,func) 					\
void conv_f32d_to_ ##dname## _shaped_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	dtype *d0 = dst[0];							\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;	\
	float *dither = conv->dither;						\
	update_dither_c(conv, SPA_MIN(n_samples, dither_size));			\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = &d0[i];						\
		struct shaper *sh = &conv->shaper[i];				\
		uint32_t idx = sh->idx;						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, dither_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j*n_channels] = func (s[j], sh, dither[k]);	\
		}								\
		sh->idx = idx;							\
	}									\
}

MAKE_D_shaped(u8, uint8_t, F32_TO_U8_SH);
MAKE_I_shaped(u8, uint8_t, F32_TO_U8_SH);
MAKE_D_shaped(s8, int8_t, F32_TO_S8_SH);
MAKE_I_shaped(s8, int8_t, F32_TO_S8_SH);
MAKE_D_shaped(s16, int16_t, F32_TO_S16_SH);
MAKE_I_shaped(s16, int16_t, F32_TO_S16_SH);
MAKE_I_shaped(s16s, uint16_t, F32_TO_S16S_SH);

#define MAKE_DEINTERLEAVE(size,type,func)					\
	MAKE_I_TO_D(size,type,size,type,func)

MAKE_DEINTERLEAVE(8, uint8_t, (uint8_t));
MAKE_DEINTERLEAVE(16, uint16_t, (uint16_t));
MAKE_DEINTERLEAVE(24, uint24_t, (uint24_t));
MAKE_DEINTERLEAVE(32, uint32_t, (uint32_t));
MAKE_DEINTERLEAVE(32s, uint32_t, bswap_32);
MAKE_DEINTERLEAVE(64, uint64_t, (uint64_t));

#define MAKE_INTERLEAVE(size,type,func)						\
	MAKE_D_TO_I(size,type,size,type,func)

MAKE_INTERLEAVE(8, uint8_t, (uint8_t));
MAKE_INTERLEAVE(16, uint16_t, (uint16_t));
MAKE_INTERLEAVE(24, uint24_t, (uint24_t));
MAKE_INTERLEAVE(32, uint32_t, (uint32_t));
MAKE_INTERLEAVE(32s, uint32_t, bswap_32);
MAKE_INTERLEAVE(64, uint64_t, (uint64_t));
