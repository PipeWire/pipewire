/* Spa BAP codec API
 *
 * Copyright © 2022 Collabora
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
#ifndef SPA_BLUEZ5_BAP_CODEC_CAPS_H_
#define SPA_BLUEZ5_BAP_CODEC_CAPS_H_

#define BAP_CODEC_LC3           0x06

#define LC3_TYPE_FREQ           0x01
#define LC3_FREQ_8KHZ           (1 << 0)
#define LC3_FREQ_11KHZ          (1 << 1)
#define LC3_FREQ_16KHZ          (1 << 2)
#define LC3_FREQ_22KHZ          (1 << 3)
#define LC3_FREQ_24KHZ          (1 << 4)
#define LC3_FREQ_32KHZ          (1 << 5)
#define LC3_FREQ_44KHZ          (1 << 6)
#define LC3_FREQ_48KHZ          (1 << 7)
#define LC3_FREQ_ANY            (LC3_FREQ_8KHZ | \
                                 LC3_FREQ_11KHZ | \
                                 LC3_FREQ_16KHZ | \
                                 LC3_FREQ_22KHZ | \
                                 LC3_FREQ_24KHZ | \
                                 LC3_FREQ_32KHZ | \
                                 LC3_FREQ_44KHZ | \
                                 LC3_FREQ_48KHZ)

#define LC3_TYPE_DUR            0x02
#define LC3_DUR_7_5             (1 << 0)
#define LC3_DUR_10              (1 << 1)
#define LC3_DUR_ANY             (LC3_DUR_7_5 | \
                                 LC3_DUR_10)

#define LC3_TYPE_CHAN           0x03
#define LC3_CHAN_1              (1 << 0)
#define LC3_CHAN_2              (1 << 1)

#define LC3_TYPE_FRAMELEN       0x04
#define LC3_TYPE_BLKS           0x05

/* LC3 config parameters */
#define LC3_CONFIG_FREQ_8KHZ    0x01
#define LC3_CONFIG_FREQ_11KHZ   0x02
#define LC3_CONFIG_FREQ_16KHZ   0x03
#define LC3_CONFIG_FREQ_22KHZ   0x04
#define LC3_CONFIG_FREQ_24KHZ   0x05
#define LC3_CONFIG_FREQ_32KHZ   0x06
#define LC3_CONFIG_FREQ_44KHZ   0x07
#define LC3_CONFIG_FREQ_48KHZ   0x08

#define LC3_CONFIG_DURATION_7_5 0x00
#define LC3_CONFIG_DURATION_10  0x01

#define LC3_CONFIG_CHNL_NOT_ALLOWED 0x00000000
#define LC3_CONFIG_CHNL_FL          0x00000001 /* front left */
#define LC3_CONFIG_CHNL_FR          0x00000002 /* front right */

#define LC3_MAX_CHANNELS 2

typedef struct {
    uint8_t rate;
	uint8_t frame_duration;
	uint32_t channels;
	uint16_t framelen;
	uint8_t n_blks;
} __attribute__ ((packed)) bap_lc3_t;

#endif
