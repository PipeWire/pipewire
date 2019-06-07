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

	struct spa_handle *system_handle;
	struct spa_handle *loop_handle;
	struct pw_properties *properties;
};
/** \endcond */

/** Create a new loop
 * \returns a newly allocated loop
 * \memberof pw_loop
 */
SPA_EXPORT
struct pw_loop *pw_loop_new(struct pw_properties *properties)
{
	int res;
	struct impl *impl;
	struct pw_loop *this;
	void *iface;
	struct spa_support support[32];
	uint32_t n_support;
	const char *lib;

	n_support = pw_get_support(support, 32);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	impl->properties = properties;

	if (properties)
		lib = pw_properties_get(properties, PW_KEY_LOOP_LIBRARY_SYSTEM);
	else
		lib = NULL;

	impl->system_handle = pw_load_spa_handle(lib,
			"system",
			properties ? &properties->dict : NULL,
			n_support, support);
	if (impl->system_handle == NULL) {
		res = -errno;
		pw_log_error("can't make system handle");
		goto out_free;
	}

        if ((res = spa_handle_get_interface(impl->system_handle,
					    SPA_TYPE_INTERFACE_System,
					    &iface)) < 0) {
                fprintf(stderr, "can't get System interface %d\n", res);
                goto out_free_system;
	}
	this->system = iface;

	support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, iface);

	if (properties)
		lib = pw_properties_get(properties, PW_KEY_LOOP_LIBRARY_LOOP);
	else
		lib = NULL;

	impl->loop_handle = pw_load_spa_handle(lib,
			"loop",
			properties ? &properties->dict : NULL,
			n_support, support);
	if (impl->loop_handle == NULL) {
		res = -errno;
		pw_log_error("can't make loop handle");
		goto out_free_system;
	}

        if ((res = spa_handle_get_interface(impl->system_handle,
					    SPA_TYPE_INTERFACE_System,
					    &iface)) < 0) {
                fprintf(stderr, "can't get System interface %d\n", res);
                goto out_free_loop;
	}
	this->system = iface;

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_Loop,
					    &iface)) < 0) {
                fprintf(stderr, "can't get Loop interface %d\n", res);
                goto out_free_loop;
        }
	this->loop = iface;

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_LoopControl,
					    &iface)) < 0) {
                fprintf(stderr, "can't get LoopControl interface %d\n", res);
                goto out_free_loop;
        }
	this->control = iface;

        if ((res = spa_handle_get_interface(impl->loop_handle,
					    SPA_TYPE_INTERFACE_LoopUtils,
					    &iface)) < 0) {
                fprintf(stderr, "can't get LoopUtils interface %d\n", res);
                goto out_free_loop;
        }
	this->utils = iface;

	return this;

      out_free_loop:
	pw_unload_spa_handle(impl->loop_handle);
      out_free_system:
	pw_unload_spa_handle(impl->system_handle);
      out_free:
	free(impl);
	errno = -res;
	return NULL;
}

/** Destroy a loop
 * \param loop a loop to destroy
 * \memberof pw_loop
 */
SPA_EXPORT
void pw_loop_destroy(struct pw_loop *loop)
{
	struct impl *impl = SPA_CONTAINER_OF(loop, struct impl, this);

	if (impl->properties)
		pw_properties_free(impl->properties);
	pw_unload_spa_handle(impl->loop_handle);
	pw_unload_spa_handle(impl->system_handle);
	free(impl);
}
