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

#ifndef __PIPEWIRE_CLIENT_STREAM_H__
#define __PIPEWIRE_CLIENT_STREAM_H__

#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_client_stream
 *
 * PipeWire client stream interface
 */
struct pw_client_stream {
	struct pw_node *node;
};

struct pw_client_stream *
pw_client_stream_new(struct pw_resource *resource,
		     struct pw_global *parent,
		     struct pw_properties *properties);

void
pw_client_stream_destroy(struct pw_client_stream *stream);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_STREAM_H__ */
