/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "fmt-ops.h"
#include "law.h"

#define MAKE_COPY(size)								\
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

#define MAKE_D_TO_D(sname,stype,dname,dtype,func)				\
void conv_ ##sname## d_to_ ##dname## d_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (i = 0; i < n_channels; i++) {					\
		const stype *s = src[i];					\
		dtype *d = dst[i];						\
		for (j = 0; j < n_samples; j++)					\
			d[j] = func (s[j]);					\
	}									\
}

#define MAKE_I_TO_I(sname,stype,dname,dtype,func)				\
void conv_ ##sname## _to_ ##dname## _c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t j;								\
	const stype *s = src[0];						\
	dtype *d = dst[0];							\
	n_samples *= conv->n_channels;						\
	for (j = 0; j < n_samples; j++)						\
		d[j] = func (s[j]);						\
}

#define MAKE_I_TO_D(sname,stype,dname,dtype,func)				\
void conv_ ##sname## _to_ ##dname## d_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const stype *s = src[0];						\
	dtype **d = (dtype**)dst;						\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (j = 0; j < n_samples; j++) {					\
		for (i = 0; i < n_channels; i++)				\
			d[i][j] = func (*s++);					\
	}									\
}

#define MAKE_D_TO_I(sname,stype,dname,dtype,func)				\
void conv_ ##sname## d_to_ ##dname## _c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const stype **s = (const stype **)src;					\
	dtype *d = dst[0];							\
	uint32_t i, j, n_channels = conv->n_channels;				\
	for (j = 0; j < n_samples; j++) {					\
		for (i = 0; i < n_channels; i++)				\
			*d++ = func (s[i][j]);					\
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
MAKE_I_TO_D(f64s, uint64_t, f32, float, (float)F64S_TO_F64);

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
MAKE_D_TO_I(f32, float, f64s, uint64_t, F64_TO_F64S);


static inline int32_t
lcnoise(uint32_t *state)
{
        *state = (*state * 96314165) + 907633515;
        return (int32_t)(*state);
}

void conv_noise_none_c(struct convert *conv, float *noise, uint32_t n_samples)
{
	memset(noise, 0, n_samples * sizeof(float));
}

void conv_noise_rect_c(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	uint32_t *state = &conv->random[0];
	const float scale = conv->scale;

	for (n = 0; n < n_samples; n++)
		noise[n] = lcnoise(state) * scale;
}

void conv_noise_tri_c(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	const float scale = conv->scale;
	uint32_t *state = &conv->random[0];

	for (n = 0; n < n_samples; n++)
		noise[n] = (lcnoise(state) - lcnoise(state)) * scale;
}

void conv_noise_tri_hf_c(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	const float scale = conv->scale;
	uint32_t *state = &conv->random[0];
	int32_t *prev = &conv->prev[0], old, new;

	old = *prev;
	for (n = 0; n < n_samples; n++) {
		new = lcnoise(state);
		noise[n] = (new - old) * scale;
		old = new;
	}
	*prev = old;
}

void conv_noise_pattern_c(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	const float scale = conv->scale;
	int32_t *prev = &conv->prev[0], old;

	old = *prev;
	for (n = 0; n < n_samples; n++)
		noise[n] = scale * (1-((old++>>10)&1));
	*prev = old;
}

#define MAKE_D_noise(dname,dtype,func)						\
void conv_f32d_to_ ##dname## d_noise_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, noise_size = conv->noise_size;	\
	float *noise = conv->noise;						\
	convert_update_noise(conv, noise, SPA_MIN(n_samples, noise_size));	\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = dst[i];						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, noise_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j] = func (s[j], noise[k]);			\
		}								\
	}									\
}

#define MAKE_I_noise(dname,dtype,func)						\
void conv_f32d_to_ ##dname## _noise_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	const float **s = (const float **) src;					\
	dtype *d = dst[0];							\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, noise_size = conv->noise_size;	\
	float *noise = conv->noise;						\
	convert_update_noise(conv, noise, SPA_MIN(n_samples, noise_size));	\
	for (j = 0; j < n_samples;) {						\
		chunk = SPA_MIN(n_samples - j, noise_size);			\
		for (k = 0; k < chunk; k++, j++) {				\
			for (i = 0; i < n_channels; i++)			\
				*d++ = func (s[i][j], noise[k]);		\
		}								\
	}									\
}

MAKE_D_noise(u8, uint8_t, F32_TO_U8_D);
MAKE_I_noise(u8, uint8_t, F32_TO_U8_D);
MAKE_D_noise(s8, int8_t, F32_TO_S8_D);
MAKE_I_noise(s8, int8_t, F32_TO_S8_D);
MAKE_D_noise(s16, int16_t, F32_TO_S16_D);
MAKE_I_noise(s16, int16_t, F32_TO_S16_D);
MAKE_I_noise(s16s, uint16_t, F32_TO_S16S_D);
MAKE_D_noise(s32, int32_t, F32_TO_S32_D);
MAKE_I_noise(s32, int32_t, F32_TO_S32_D);
MAKE_I_noise(s32s, uint32_t, F32_TO_S32S_D);
MAKE_D_noise(s24, int24_t, F32_TO_S24_D);
MAKE_I_noise(s24, int24_t, F32_TO_S24_D);
MAKE_I_noise(s24s, int24_t, F32_TO_S24_D);
MAKE_D_noise(s24_32, int32_t, F32_TO_S24_32_D);
MAKE_I_noise(s24_32, int32_t, F32_TO_S24_32_D);
MAKE_I_noise(s24_32s, int32_t, F32_TO_S24_32S_D);

#define SHAPER(type,s,scale,offs,sh,min,max,d)			\
({								\
	type t;							\
	float v = s * scale + offs;				\
	for (n = 0; n < n_ns; n++)				\
		v += sh->e[idx + n] * ns[n];			\
	t = FTOI(type, v, 1.0f, 0.0f, d, min, max);		\
	idx = (idx - 1) & NS_MASK;				\
	sh->e[idx] = sh->e[idx + NS_MAX] = v - t;		\
	t;							\
})

#define F32_TO_U8_SH(s,sh,d)	SHAPER(uint8_t, s, U8_SCALE, U8_OFFS, sh, U8_MIN, U8_MAX, d)
#define F32_TO_S8_SH(s,sh,d)	SHAPER(int8_t, s, S8_SCALE, 0, sh, S8_MIN, S8_MAX, d)
#define F32_TO_S16_SH(s,sh,d)	SHAPER(int16_t, s, S16_SCALE, 0, sh, S16_MIN, S16_MAX, d)
#define F32_TO_S16S_SH(s,sh,d)	bswap_16(F32_TO_S16_SH(s,sh,d))

#define MAKE_D_shaped(dname,dtype,func)						\
void conv_f32d_to_ ##dname## d_shaped_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, noise_size = conv->noise_size;	\
	float *noise = conv->noise;						\
	const float *ns = conv->ns;						\
	uint32_t n, n_ns = conv->n_ns;						\
	convert_update_noise(conv, noise, SPA_MIN(n_samples, noise_size));	\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = dst[i];						\
		struct shaper *sh = &conv->shaper[i];				\
		uint32_t idx = sh->idx;						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, noise_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j] = func (s[j], sh, noise[k]);		\
		}								\
		sh->idx = idx;							\
	}									\
}

#define MAKE_I_shaped(dname,dtype,func)						\
void conv_f32d_to_ ##dname## _shaped_c(struct convert *conv,			\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
                uint32_t n_samples)						\
{										\
	dtype *d0 = dst[0];							\
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, noise_size = conv->noise_size;	\
	float *noise = conv->noise;						\
	const float *ns = conv->ns;						\
	uint32_t n, n_ns = conv->n_ns;						\
	convert_update_noise(conv, noise, SPA_MIN(n_samples, noise_size));	\
	for (i = 0; i < n_channels; i++) {					\
		const float *s = src[i];					\
		dtype *d = &d0[i];						\
		struct shaper *sh = &conv->shaper[i];				\
		uint32_t idx = sh->idx;						\
		for (j = 0; j < n_samples;) {					\
			chunk = SPA_MIN(n_samples - j, noise_size);		\
			for (k = 0; k < chunk; k++, j++)			\
				d[j*n_channels] = func (s[j], sh, noise[k]);	\
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

#define MAKE_DEINTERLEAVE(size1,size2, type,func)					\
	MAKE_I_TO_D(size1,type,size2,type,func)

MAKE_DEINTERLEAVE(8, 8, uint8_t, (uint8_t));
MAKE_DEINTERLEAVE(16, 16, uint16_t, (uint16_t));
MAKE_DEINTERLEAVE(24, 24, uint24_t, (uint24_t));
MAKE_DEINTERLEAVE(32, 32, uint32_t, (uint32_t));
MAKE_DEINTERLEAVE(32s, 32, uint32_t, bswap_32);
MAKE_DEINTERLEAVE(64, 64, uint64_t, (uint64_t));

#define MAKE_INTERLEAVE(size1,size2,type,func)						\
	MAKE_D_TO_I(size1,type,size2,type,func)

MAKE_INTERLEAVE(8, 8, uint8_t, (uint8_t));
MAKE_INTERLEAVE(16, 16, uint16_t, (uint16_t));
MAKE_INTERLEAVE(24, 24, uint24_t, (uint24_t));
MAKE_INTERLEAVE(32, 32, uint32_t, (uint32_t));
MAKE_INTERLEAVE(32, 32s, uint32_t, bswap_32);
MAKE_INTERLEAVE(64, 64, uint64_t, (uint64_t));
