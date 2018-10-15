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
	   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **)dst;
	float **s = (float **)src;
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
		for (i = 0; i < n_dst; i++) {
			float *di = d[i], *si = s[i];

			unrolled = n_samples / 4;
			remain = n_samples & 3;

			for(n = 0; unrolled--; n += 4)
				_mm_storeu_ps(&di[n], _mm_mul_ps(_mm_loadu_ps(&si[n]), vol));
			for(; remain--; n++)
				_mm_store_ss(&di[n], _mm_mul_ss(_mm_load_ss(&si[n]), vol));
		}
	}
}

static void
channelmix_f32_2_4_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **)dst;
	float **s = (float **)src;
        __m128 vol = _mm_set1_ps(v);
	__m128 in;
	float *dFL = d[0], *dFR = d[1], *dRL = d[2], *dRR = d[3];
	float *sFL = s[0], *sFR = s[1];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			in = _mm_loadu_ps(&sFL[n]);
			_mm_storeu_ps(&dFL[n], in);
			_mm_storeu_ps(&dRL[n], in);
			in = _mm_loadu_ps(&sFR[n]);
			_mm_storeu_ps(&dFR[n], in);
			_mm_storeu_ps(&dRR[n], in);
		}
		for(; remain--; n++) {
			in = _mm_load_ss(&sFL[n]);
			_mm_store_ss(&dFL[n], in);
			_mm_store_ss(&dRL[n], in);
			in = _mm_load_ss(&sFR[n]);
			_mm_store_ss(&dFR[n], in);
			_mm_store_ss(&dRR[n], in);
		}
	}
	else {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			in = _mm_mul_ps(_mm_loadu_ps(&sFL[n]), vol);
			_mm_storeu_ps(&dFL[n], in);
			_mm_storeu_ps(&dRL[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&sFR[n]), vol);
			_mm_storeu_ps(&dFR[n], in);
			_mm_storeu_ps(&dRR[n], in);
		}
		for(; remain--; n++) {
			in = _mm_mul_ss(_mm_load_ss(&sFL[n]), vol);
			_mm_store_ss(&dFL[n], in);
			_mm_store_ss(&dRL[n], in);
			in = _mm_mul_ss(_mm_load_ss(&sFR[n]), vol);
			_mm_store_ss(&dFR[n], in);
			_mm_store_ss(&dRR[n], in);
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR */
static void
channelmix_f32_5p1_2_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
        __m128 clev = _mm_set1_ps(m[2]);
        __m128 llev = _mm_set1_ps(m[3]);
        __m128 slev = _mm_set1_ps(m[4]);
        __m128 vol = _mm_set1_ps(v);
	__m128 in, ctr;
	float *dFL = d[0], *dFR = d[1];
	float *sFL = s[0], *sFR = s[1], *sFC = s[2], *sLFE = s[3], *sSL = s[4], *sSR = s[5];

	if (v <= VOLUME_MIN) {
		memset(dFL, 0, n_bytes);
		memset(dFR, 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {

		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			in = _mm_mul_ps(_mm_loadu_ps(&sSL[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&sFL[n]));
			_mm_storeu_ps(&dFL[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&sSR[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&sFR[n]));
			_mm_storeu_ps(&dFR[n], in);
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			in = _mm_mul_ss(_mm_load_ss(&sSL[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&sFL[n]));
			_mm_store_ss(&dFL[n], in);
			in = _mm_mul_ss(_mm_load_ss(&sSR[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&sFR[n]));
			_mm_store_ss(&dFR[n], in);
		}
	}
	else {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			in = _mm_mul_ps(_mm_loadu_ps(&sSL[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&sFL[n]));
			in = _mm_mul_ps(in, vol);
			_mm_storeu_ps(&dFL[n], in);
			in = _mm_mul_ps(_mm_loadu_ps(&sSR[n]), slev);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_loadu_ps(&sFR[n]));
			in = _mm_mul_ps(in, vol);
			_mm_storeu_ps(&dFR[n], in);
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			in = _mm_mul_ss(_mm_load_ss(&sSL[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&sFL[n]));
			in = _mm_mul_ss(in, vol);
			_mm_store_ss(&dFL[n], in);
			in = _mm_mul_ss(_mm_load_ss(&sSR[n]), slev);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_load_ss(&sFR[n]));
			in = _mm_mul_ss(in, vol);
			_mm_store_ss(&dFR[n], in);
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+RL+RR*/
static void
channelmix_f32_5p1_4_sse(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float), unrolled, remain;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
        __m128 clev = _mm_set1_ps(m[2]);
        __m128 llev = _mm_set1_ps(m[3]);
        __m128 vol = _mm_set1_ps(v);
	__m128 ctr;
	float *dFL = d[0], *dFR = d[1], *dRL = d[2], *dRR = d[3];
	float *sFL = s[0], *sFR = s[1], *sFC = s[2], *sLFE = s[3], *sSL = s[4], *sSR = s[5];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			_mm_storeu_ps(&dFL[n], _mm_add_ps(_mm_loadu_ps(&sFL[n]), ctr));
			_mm_storeu_ps(&dFR[n], _mm_add_ps(_mm_loadu_ps(&sFR[n]), ctr));
			_mm_storeu_ps(&dRL[n], _mm_loadu_ps(&sSL[n]));
			_mm_storeu_ps(&dRR[n], _mm_loadu_ps(&sSR[n]));
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			_mm_store_ss(&dFL[n], _mm_add_ss(_mm_load_ss(&sFL[n]), ctr));
			_mm_store_ss(&dFR[n], _mm_add_ss(_mm_load_ss(&sFR[n]), ctr));
			_mm_store_ss(&dRL[n], _mm_load_ss(&sSL[n]));
			_mm_store_ss(&dRR[n], _mm_load_ss(&sSR[n]));
		}
	}
	else {
		unrolled = n_samples / 4;
		remain = n_samples & 3;

		for(n = 0; unrolled--; n += 4) {
			ctr = _mm_mul_ps(_mm_loadu_ps(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			_mm_storeu_ps(&dFL[n], _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(&sFL[n]), ctr), vol));
			_mm_storeu_ps(&dFR[n], _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(&sFR[n]), ctr), vol));
			_mm_storeu_ps(&dRL[n], _mm_mul_ps(_mm_loadu_ps(&sSL[n]), vol));
			_mm_storeu_ps(&dRR[n], _mm_mul_ps(_mm_loadu_ps(&sSR[n]), vol));
		}
		for(; remain--; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&sFC[n]), clev);
			ctr = _mm_add_ps(ctr, _mm_mul_ps(_mm_loadu_ps(&sLFE[n]), llev));
			_mm_store_ss(&dFL[n], _mm_mul_ss(_mm_add_ss(_mm_load_ss(&sFL[n]), ctr), vol));
			_mm_store_ss(&dFR[n], _mm_mul_ss(_mm_add_ss(_mm_load_ss(&sFR[n]), ctr), vol));
			_mm_store_ss(&dRL[n], _mm_mul_ss(_mm_load_ss(&sSL[n]), vol));
			_mm_store_ss(&dRR[n], _mm_mul_ss(_mm_load_ss(&sSR[n]), vol));
		}
	}
}
