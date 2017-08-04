/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>

#include <spa/loop.h>
#include <spa/type-map.h>

#include <pipewire/pipewire.h>
#include <pipewire/loop.h>
#include <pipewire/log.h>

#define DATAS_SIZE (4096 * 8)

/** \cond */

struct impl {
	struct pw_loop this;

	struct spa_handle *handle;
};
/** \endcond */

/** Create a new loop
 * \returns a newly allocated loop
 * \memberof pw_loop
 */
struct pw_loop *pw_loop_new(void)
{
	int res;
	struct impl *impl;
	struct pw_loop *this;
	const struct spa_handle_factory *factory;
	struct spa_type_map *map;
	void *iface;
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_get_support(&n_support);
	if (support == NULL)
		return NULL;

        map = spa_support_find(support, n_support, SPA_TYPE__TypeMap);
	if (map == NULL)
		return NULL;

	factory = pw_get_support_factory("loop");
	if (factory == NULL)
		return NULL;

	impl = calloc(1, sizeof(struct impl) + factory->size);
	if (impl == NULL)
		return NULL;

	impl->handle = SPA_MEMBER(impl, sizeof(struct impl), struct spa_handle);

	this = &impl->this;

	if ((res = spa_handle_factory_init(factory,
					   impl->handle,
					   NULL,
					   support,
					   n_support)) < 0) {
		fprintf(stderr, "can't make factory instance: %d\n", res);
		goto failed;
	}

        if ((res = spa_handle_get_interface(impl->handle,
					    spa_type_map_get_id(map, SPA_TYPE__Loop),
					    &iface)) < 0) {
                fprintf(stderr, "can't get %s interface %d\n", SPA_TYPE__Loop, res);
                goto failed;
        }
	this->loop = iface;

        if ((res = spa_handle_get_interface(impl->handle,
					    spa_type_map_get_id(map, SPA_TYPE__LoopControl),
					    &iface)) < 0) {
                fprintf(stderr, "can't get %s interface %d\n", SPA_TYPE__LoopControl, res);
                goto failed;
        }
	this->control = iface;

        if ((res = spa_handle_get_interface(impl->handle,
					    spa_type_map_get_id(map, SPA_TYPE__LoopUtils),
					    &iface)) < 0) {
                fprintf(stderr, "can't get %s interface %d\n", SPA_TYPE__LoopUtils, res);
                goto failed;
        }
	this->utils = iface;

	return this;

      failed:
	free(impl);
	return NULL;
}

/** Destroy a loop
 * \param loop a loop to destroy
 * \memberof pw_loop
 */
void pw_loop_destroy(struct pw_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	spa_handle_clear(impl->handle);
	free(impl);
}
