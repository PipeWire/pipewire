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

#ifndef PIPEWIRE_SPA_MONITOR_H
#define PIPEWIRE_SPA_MONITOR_H

#include <spa/monitor/monitor.h>

#include <pipewire/core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_spa_monitor {
	struct spa_monitor *monitor;

	char *factory_name;
	char *system_name;
	struct spa_handle *handle;
	struct pw_properties *properties;

	void *user_data;
};

struct pw_spa_monitor *
pw_spa_monitor_load(struct pw_core *core,
		    struct pw_global *parent,
		    const char *factory_name,
		    const char *system_name,
		    struct pw_properties *properties,
		    size_t user_data_size);
void
pw_spa_monitor_destroy(struct pw_spa_monitor *monitor);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_SPA_MONITOR_H */
