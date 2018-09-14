/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <xmmintrin.h>

static void
channelmix_copy_sse(void *data, int n_dst, void *dst[n_dst],
	   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];
        __m128 vol = _mm_set1_ps(v);

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (i = 0; i < n_dst; i++)
			memcpy(d[i], s[i], n_bytes);
	}
	else {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for (i = 0; i < n_dst; i++) {
			float *di = d[i], *si = s[i];
			for(n = 0; unrolled--; n += 4)
				_mm_storeu_ps(&di[n], _mm_mul_ps(_mm_loadu_ps(&si[n]), vol));
			for(; remain--; n++)
				_mm_store_ss(&di[n], _mm_mul_ss(_mm_load_ss(&si[n]), vol));
		}
	}
}

static void
channelmix_f32_2_4_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];
        __m128 vol = _mm_set1_ps(v);
	__m128 in;

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		float *d0 = d[0], *d1 = d[1], *d2 = d[2], *d3 = d[3], *s0 = s[0], *s1 = s[1];
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			in = _mm_loadu_ps(&s0[n]);
			_mm_storeu_ps(&d0[n], in);
			_mm_storeu_ps(&d2[n], in);
			in = _mm_loadu_ps(&s1[n]);
			_mm_storeu_ps(&d1[n], in);
			_mm_storeu_ps(&d3[n], in);
		}
		for(; remain--; n++) {
			in = _mm_load_ss(&s0[n]);
			_mm_store_ss(&d0[n], in);
			_mm_store_ss(&d2[n], in);
			in = _mm_load_ss(&s1[n]);
			_mm_store_ss(&d1[n], in);
			_mm_store_ss(&d3[n], in);
		}
	}
	else {
		float *d0 = d[0], *d1 = d[1], *d2 = d[2], *d3 = d[3], *s0 = s[0], *s1 = s[1];
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			in = _mm_mul_ps(_mm_loadu_ps(&s0[n]), vol);
			_mm_storeu_ps(&d0[n], in);
			_mm_storeu_ps(&d2[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&s1[n]), vol);
			_mm_storeu_ps(&d1[n], in);
			_mm_storeu_ps(&d3[n], in);
		}
		for(; remain--; n++) {
			in = _mm_mul_ss(_mm_load_ss(&s0[n]), vol);
			_mm_store_ss(&d0[n], in);
			_mm_store_ss(&d2[n], in);
			in = _mm_mul_ss(_mm_load_ss(&s1[n]), vol);
			_mm_store_ss(&d1[n], in);
			_mm_store_ss(&d3[n], in);
		}
	}
}

/* FL+FR+RL+RR+FC+LFE -> FL+FR */
static void
channelmix_f32_5p1_2_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	float v = m[0];
        __m128 clev = _mm_set1_ps(0.7071f);
        __m128 slev = _mm_set1_ps(0.7071f);
        __m128 vol = _mm_set1_ps(v);
	__m128 in, ctr;

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
		memset(d[1], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		float *d0 = d[0], *d1 = d[1], *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3], *s4 = s[4];

		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&s4[n]), clev);
			in = _mm_mul_ps(_mm_loadu_ps(&s2[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&s0[n]));
			_mm_storeu_ps(&d0[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&s3[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&s1[n]));
			_mm_storeu_ps(&d1[n], in);
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s4[n]), clev);
			in = _mm_mul_ss(_mm_load_ss(&s2[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&s0[n]));
			_mm_store_ss(&d0[n], in);
			in = _mm_mul_ss(_mm_load_ss(&s3[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&s1[n]));
			_mm_store_ss(&d1[n], in);
		}
	}
	else {
		float *d0 = d[0], *d1 = d[1], *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3], *s4 = s[4];

		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&s4[n]), clev);
			in = _mm_mul_ps(_mm_loadu_ps(&s2[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&s0[n]));
			in = _mm_mul_ps(in, vol);
			_mm_storeu_ps(&d0[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&s3[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&s1[n]));
			in = _mm_mul_ps(in, vol);
			_mm_storeu_ps(&d1[n], in);
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s4[n]), clev);
			in = _mm_mul_ss(_mm_load_ss(&s2[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&s0[n]));
			in = _mm_mul_ss(in, vol);
			_mm_store_ss(&d0[n], in);
			in = _mm_mul_ss(_mm_load_ss(&s3[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&s1[n]));
			in = _mm_mul_ss(in, vol);
			_mm_store_ss(&d1[n], in);
		}
	}
}

/* FL+FR+RL+RR+FC+LFE -> FL+FR+RL+RR*/
static void
channelmix_f32_5p1_4_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	float v = m[0];
        __m128 clev = _mm_set1_ps(0.7071f);
        __m128 vol = _mm_set1_ps(v);
	__m128 ctr;

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		float *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3], *s4 = s[4];
		float *d0 = d[0], *d1 = d[1], *d2 = d[2], *d3 = d[3];

		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&s4[n]), clev);
			_mm_storeu_ps(&d0[n], _mm_add_ps(_mm_loadu_ps(&s0[n]), ctr));
			_mm_storeu_ps(&d1[n], _mm_add_ps(_mm_loadu_ps(&s1[n]), ctr));
			_mm_storeu_ps(&d2[n], _mm_loadu_ps(&s2[n]));
			_mm_storeu_ps(&d3[n], _mm_loadu_ps(&s3[n]));
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s4[n]), clev);
			_mm_store_ss(&d0[n], _mm_add_ss(_mm_load_ss(&s0[n]), ctr));
			_mm_store_ss(&d1[n], _mm_add_ss(_mm_load_ss(&s1[n]), ctr));
			_mm_store_ss(&d2[n], _mm_load_ss(&s2[n]));
			_mm_store_ss(&d3[n], _mm_load_ss(&s3[n]));
		}
	}
	else {
		float *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3], *s4 = s[4];
		float *d0 = d[0], *d1 = d[1], *d2 = d[2], *d3 = d[3];

		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&s4[n]), clev);
			_mm_storeu_ps(&d0[n], _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(&s0[n]), ctr), vol));
			_mm_storeu_ps(&d1[n], _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(&s1[n]), ctr), vol));
			_mm_storeu_ps(&d2[n], _mm_mul_ps(_mm_loadu_ps(&s2[n]), vol));
			_mm_storeu_ps(&d3[n], _mm_mul_ps(_mm_loadu_ps(&s3[n]), vol));
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s4[n]), clev);
			_mm_store_ss(&d0[n], _mm_mul_ss(_mm_add_ss(_mm_load_ss(&s0[n]), ctr), vol));
			_mm_store_ss(&d1[n], _mm_mul_ss(_mm_add_ss(_mm_load_ss(&s1[n]), ctr), vol));
			_mm_store_ss(&d2[n], _mm_mul_ss(_mm_load_ss(&s2[n]), vol));
			_mm_store_ss(&d3[n], _mm_mul_ss(_mm_load_ss(&s3[n]), vol));
		}
	}
}
