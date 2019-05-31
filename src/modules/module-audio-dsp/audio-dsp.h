/* PipeWire
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

#ifndef PIPEWIRE_AUDIO_DSP_H
#define PIPEWIRE_AUDIO_DSP_H

#include <pipewire/core.h>
#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DSP_USAGE	"audio-dsp.direction=<enum spa_direction> "	\
			"audio-dsp.maxbuffer=<int> "			\
			"audio-dsp.name=<string> "			\
			PW_KEY_DEVICE_API"=<string> "			\
			"["PW_KEY_NODE_ID"=<int>]"

struct pw_node *
pw_audio_dsp_new(struct pw_core *core,
		 const struct pw_properties *properties,
		 size_t user_data_size);

void *pw_audio_dsp_get_user_data(struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_AUDIO_DSP_H */
