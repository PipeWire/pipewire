/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
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


#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <spa/pod/pod.h>
#include <spa/param/audio/raw.h>
#include <spa/support/plugin.h>

#define SPA_TYPE_INTERFACE_AEC SPA_TYPE_INFO_INTERFACE_BASE "AEC"
#define SPA_VERSION_AUDIO_AEC   1

struct echo_cancel_info {
	struct spa_interface iface;
	const char *name;
	const struct spa_dict info;
	const char *latency;
	int (*create) (struct spa_handle *handle, const struct spa_dict *args, const struct spa_audio_info_raw *info);
	int (*run) (struct spa_handle *handle, const float *rec[], const float *play[], float *out[], uint32_t n_samples);
	struct spa_dict *(*get_properties) (struct spa_handle *handle);
	int (*set_properties) (struct spa_handle *handle, const struct spa_dict *args);
};
