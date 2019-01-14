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

#ifndef PIPEWIRE_EXT_PROTOCOL_NATIVE_H
#define PIPEWIRE_EXT_PROTOCOL_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/node/node.h>

#define PW_TYPE_INFO_PROTOCOL_Native		PW_TYPE_INFO_PROTOCOL_BASE "Native"

struct pw_protocol_native_demarshal {
	int (*func) (void *object, void *data, size_t size);
	uint32_t permissions;
};

/** \ref pw_protocol_native_ext methods */
struct pw_protocol_native_ext {
#define PW_VERSION_PROTOCOL_NATIVE_EXT	0
	uint32_t version;

	struct spa_pod_builder * (*begin_proxy) (struct pw_proxy *proxy,
						 uint8_t opcode);

	uint32_t (*add_proxy_fd) (struct pw_proxy *proxy, int fd);
	int (*get_proxy_fd) (struct pw_proxy *proxy, uint32_t index);

	void (*end_proxy) (struct pw_proxy *proxy,
			   struct spa_pod_builder *builder);

	struct spa_pod_builder * (*begin_resource) (struct pw_resource *resource,
						    uint8_t opcode);

	uint32_t (*add_resource_fd) (struct pw_resource *resource, int fd);
	int (*get_resource_fd) (struct pw_resource *resource, uint32_t index);

	void (*end_resource) (struct pw_resource *resource,
			      struct spa_pod_builder *builder);

};

#define pw_protocol_native_begin_proxy(p,...)		pw_protocol_ext(pw_proxy_get_protocol(p),struct pw_protocol_native_ext,begin_proxy,p,__VA_ARGS__)
#define pw_protocol_native_add_proxy_fd(p,...)		pw_protocol_ext(pw_proxy_get_protocol(p),struct pw_protocol_native_ext,add_proxy_fd,p,__VA_ARGS__)
#define pw_protocol_native_get_proxy_fd(p,...)		pw_protocol_ext(pw_proxy_get_protocol(p),struct pw_protocol_native_ext,get_proxy_fd,p,__VA_ARGS__)
#define pw_protocol_native_end_proxy(p,...)		pw_protocol_ext(pw_proxy_get_protocol(p),struct pw_protocol_native_ext,end_proxy,p,__VA_ARGS__)

#define pw_protocol_native_begin_resource(r,...)	pw_protocol_ext(pw_resource_get_protocol(r),struct pw_protocol_native_ext,begin_resource,r,__VA_ARGS__)
#define pw_protocol_native_add_resource_fd(r,...)	pw_protocol_ext(pw_resource_get_protocol(r),struct pw_protocol_native_ext,add_resource_fd,r,__VA_ARGS__)
#define pw_protocol_native_get_resource_fd(r,...)	pw_protocol_ext(pw_resource_get_protocol(r),struct pw_protocol_native_ext,get_resource_fd,r,__VA_ARGS__)
#define pw_protocol_native_end_resource(r,...)		pw_protocol_ext(pw_resource_get_protocol(r),struct pw_protocol_native_ext,end_resource,r,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_PROTOCOL_NATIVE_H */
