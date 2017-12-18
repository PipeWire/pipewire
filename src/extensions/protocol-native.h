/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_EXT_PROTOCOL_NATIVE_H__
#define __PIPEWIRE_EXT_PROTOCOL_NATIVE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/node/node.h>

#define PW_TYPE_PROTOCOL__Native	PW_TYPE_PROTOCOL_BASE "Native"
#define PW_TYPE_PROTOCOL_NATIVE_BASE	PW_TYPE_PROTOCOL__Native ":"

struct pw_protocol_native_demarshal {
	int (*func) (void *object, void *data, size_t size);

#define PW_PROTOCOL_NATIVE_REMAP	(1<<0)
#define PW_PROTOCOL_NATIVE_PERM_W	(1<<1)
	uint32_t flags;
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

#endif /* __PIPEWIRE_EXT_PROTOCOL_NATIVE_H__ */
