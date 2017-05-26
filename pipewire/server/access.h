/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_ACCESS_H__
#define __PIPEWIRE_ACCESS_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Access                         "PipeWire:Object:Access"
#define PIPEWIRE_TYPE_ACCESS_BASE                      PIPEWIRE_TYPE__Access ":"

#include <pipewire/client/sig.h>
#include <pipewire/server/client.h>
#include <pipewire/server/resource.h>

struct pw_access_data {
	int res;
	struct pw_resource *resource;

	void *(*async_copy) (struct pw_access_data *data, size_t size);
	void (*complete_cb) (struct pw_access_data *data);
	void (*free_cb) (struct pw_access_data *data);
	void *user_data;
};


/**
 * struct pw_access:
 *
 * PipeWire Access support struct.
 */
struct pw_access {
	int (*view_global) (struct pw_access *access,
			    struct pw_client *client, struct pw_global *global);
	int (*create_node) (struct pw_access *access,
			    struct pw_access_data *data,
			    const char *factory_name,
			    const char *name, struct pw_properties *properties);
	int (*create_client_node) (struct pw_access *access,
				   struct pw_access_data *data,
				   const char *name, struct pw_properties *properties);
};

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_ACCESS_H__ */
