/*
 * SpanDSP - a series of DSP components for telephony
 *
 * g722.h - The ITU G.722 codec.
 *
 * Written by Steve Underwood <steveu@coppice.org>
 *
 * Copyright (C) 2005 Steve Underwood
 *
 *  Despite my general liking of the GPL, I place my own contributions
 *  to this code in the public domain for the benefit of all mankind -
 *  even the slimy ones who might try to proprietize my work and use it
 *  to my detriment.
 *
 * Based on a single channel G.722 codec which is:
 *
 *****    Copyright (c) CMU    1993      *****
 * Computer Science, Speech Group
 * Chengxiang Lu and Alex Hauptmann
 *
 * $Id: g722.h 48959 2006-12-25 06:42:15Z rizzo $
 */

/*! \file */

#if !defined(_G722_H_)
#define _G722_H_

#include <stdint.h>

/*! \page g722_page G.722 encoding and decoding
\section g722_page_sec_1 What does it do?
The G.722 module is a bit exact implementation of the ITU G.722 specification for all three
specified bit rates - 64000bps, 56000bps and 48000bps. It passes the ITU tests.

To allow fast and flexible interworking with narrow band telephony, the encoder and decoder
support an option for the linear audio to be an 8k samples/second stream. In this mode the
codec is considerably faster, and still fully compatible with wideband terminals using G.722.

\section g722_page_sec_2 How does it work?
???.
*/

/* Format DAC12 is added to decode directly into samples suitable for
   a 12-bit DAC using offset binary representation. */

enum {
  G722_SAMPLE_RATE_8000 = 0x0001,
  G722_PACKED = 0x0002,
  G722_FORMAT_DAC12 = 0x0004,
};

#ifdef BUILD_FEATURE_DAC
#define NLDECOMPRESS_APPLY_GAIN(s, g) (((s) * (int32_t)(g)) >> 16)
// Equivalent to shift 16, add 0x8000, shift 4
#define NLDECOMPRESS_APPLY_GAIN_CONVERTED_DAC(s, g) \
  (uint16_t)((uint16_t)(((s) * (int32_t)(g)) >> 20) + 0x800)
#else
#define NLDECOMPRESS_APPLY_GAIN(s, g) (((int32_t)(s) * (int32_t)(g)) >> 16)
#endif

#ifdef BUILD_FEATURE_DAC
#define NLDECOMPRESS_PREPROCESS_PCM_SAMPLE_WITH_GAIN(s, g) \
  NLDECOMPRESS_APPLY_GAIN_CONVERTED_DAC((s), (g))
#define NLDECOMPRESS_PREPROCESS_SAMPLE_WITH_GAIN(s, g) ((int16_t)NLDECOMPRESS_APPLY_GAIN((s), (g)))
#else
#define NLDECOMPRESS_PREPROCESS_PCM_SAMPLE_WITH_GAIN NLDECOMPRESS_PREPROCESS_SAMPLE_WITH_GAIN
#define NLDECOMPRESS_PREPROCESS_SAMPLE_WITH_GAIN(s, g) \
  ((int16_t)(NLDECOMPRESS_APPLY_GAIN((s), (g))))
#endif

typedef struct {
  int s;
  int sp;
  int sz;
  int r[3];
  int a[3];
  int ap[3];
  int p[3];
  int d[7];
  int b[7];
  int bp[7];
  int nb;
  int det;
} g722_band_t;

typedef struct {
  /*! TRUE if the operating in the special ITU test mode, with the band split filters disabled. */
  int itu_test_mode;
  /*! TRUE if the G.722 data is packed */
  int packed;
  /*! TRUE if encode from 8k samples/second */
  int eight_k;
  /*! 6 for 48000kbps, 7 for 56000kbps, or 8 for 64000kbps. */
  int bits_per_sample;

  /*! Signal history for the QMF */
  int x[24];

  g722_band_t band[2];

  unsigned int in_buffer;
  int in_bits;
  unsigned int out_buffer;
  int out_bits;
} g722_encode_state_t;

typedef struct {
  /*! TRUE if the operating in the special ITU test mode, with the band split filters disabled. */
  int itu_test_mode;
  /*! TRUE if the G.722 data is packed */
  int packed;
  /*! TRUE if decode to 8k samples/second */
  int eight_k;
  /*! 6 for 48000kbps, 7 for 56000kbps, or 8 for 64000kbps. */
  int bits_per_sample;
  /*! TRUE if offset binary for a 12-bit DAC */
  int dac_pcm;

  /*! Signal history for the QMF */
  int x[24];

  g722_band_t band[2];

  unsigned int in_buffer;
  int in_bits;
  unsigned int out_buffer;
  int out_bits;
} g722_decode_state_t;

#ifdef __cplusplus
extern "C" {
#endif

g722_encode_state_t *g722_encode_init(g722_encode_state_t *s, unsigned int rate, int options);
int g722_encode_release(g722_encode_state_t *s);
int g722_encode(g722_encode_state_t *s, uint8_t g722_data[], const int16_t amp[], int len);

g722_decode_state_t *g722_decode_init(g722_decode_state_t *s, unsigned int rate, int options);
int g722_decode_release(g722_decode_state_t *s);
uint32_t g722_decode(g722_decode_state_t *s, int16_t amp[], const uint8_t g722_data[], int len,
                     uint16_t aGain);

#ifdef __cplusplus
}
#endif

#endif
