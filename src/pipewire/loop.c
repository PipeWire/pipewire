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

#include <stdio.h>

#include <spa/support/loop.h>

#include <pipewire/pipewire.h>
#include <pipewire/loop.h>
#include <pipewire/log.h>
#include <pipewire/type.h>

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
struct pw_loop *pw_loop_new(struct pw_properties *properties)
{
	int res;
	struct impl *impl;
	struct pw_loop *this;
	const struct spa_handle_factory *factory;
	void *iface;
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_get_support(&n_support);
	if (support == NULL)
		return NULL;

	factory = pw_get_support_factory("loop");
	if (factory == NULL)
		return NULL;

	impl = calloc(1, sizeof(struct impl) + spa_handle_factory_get_size(factory, NULL));
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
					    SPA_TYPE_INTERFACE_Loop,
					    &iface)) < 0) {
                fprintf(stderr, "can't get Loop interface %d\n", res);
                goto failed;
        }
	this->loop = iface;

        if ((res = spa_handle_get_interface(impl->handle,
					    SPA_TYPE_INTERFACE_LoopControl,
					    &iface)) < 0) {
                fprintf(stderr, "can't get LoopControl interface %d\n", res);
                goto failed;
        }
	this->control = iface;

        if ((res = spa_handle_get_interface(impl->handle,
					    SPA_TYPE_INTERFACE_LoopUtils,
					    &iface)) < 0) {
                fprintf(stderr, "can't get LoopUtils interface %d\n", res);
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
