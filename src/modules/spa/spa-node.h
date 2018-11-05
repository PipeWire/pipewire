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

#ifndef __PIPEWIRE_SPA_NODE_H__
#define __PIPEWIRE_SPA_NODE_H__

#include <spa/node/node.h>

#include <pipewire/core.h>
#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pw_spa_node_flags {
	PW_SPA_NODE_FLAG_ASYNC		= (1 << 0),
	PW_SPA_NODE_FLAG_DISABLE	= (1 << 1),
	PW_SPA_NODE_FLAG_ACTIVATE	= (1 << 2),
	PW_SPA_NODE_FLAG_NO_REGISTER	= (1 << 3),
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_client *owner,	/**< optional owner */
		struct pw_global *parent,	/**< optional parent */
		const char *name,
		enum pw_spa_node_flags flags,
		struct spa_node *node,
		struct spa_handle *handle,
		struct pw_properties *properties,
		size_t user_data_size);

struct pw_node *
pw_spa_node_load(struct pw_core *core,
		 struct pw_client *owner,	/**< optional owner */
		 struct pw_global *parent,	/**< optional parent */
		 const char *lib,
		 const char *factory_name,
		 const char *name,
		 enum pw_spa_node_flags flags,
		 struct pw_properties *properties,
		 size_t user_data_size);

void *pw_spa_node_get_user_data(struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_SPA_NODE_H__ */
