/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/pod/filter.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>
#include "pipewire/array.h"
#include "pipewire/core.h"

#include <pipewire/extensions/metadata.h>

#define NAME "settings"

struct impl {
	struct pw_context *context;
	struct pw_impl_metadata *metadata;

	struct spa_hook metadata_listener;
};

static void metadata_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->metadata_listener);
	impl->metadata = NULL;
}

static int metadata_property(void *data, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	struct impl *impl = data;
	struct pw_context *context = impl->context;
	struct settings *d = &context->defaults;
	struct settings *s = &context->settings;
	uint32_t v;
	bool recalc = false;

	if (subject != PW_ID_CORE)
		return 0;

	if (spa_streq(key, "log.level")) {
		v = value ? atoi(value) : 3;
		pw_log_set_level(v);
	} else if (spa_streq(key, "clock.min-quantum")) {
		v = value ? atoi(value) : 0;
		s->clock_min_quantum = v == 0 ? d->clock_min_quantum : v;
		recalc = true;
	} else if (spa_streq(key, "clock.max-quantum")) {
		v = value ? atoi(value) : 0;
		s->clock_max_quantum = v == 0 ? d->clock_max_quantum : v;
		recalc = true;
	} else if (spa_streq(key, "clock.force-rate")) {
		v = value ? atoi(value) : 0;
		s->clock_force_rate = v;
		recalc = true;
	} else if (spa_streq(key, "clock.force-quantum")) {
		v = value ? atoi(value) : 0;
		s->clock_force_quantum = SPA_MIN(v, 8192u);
		recalc = true;
	}
	if (recalc)
		pw_context_recalc_graph(context, "properties changed");

	return 0;
}

static const struct pw_impl_metadata_events metadata_events = {
	PW_VERSION_IMPL_METADATA_EVENTS,
	.destroy = metadata_destroy,
	.property = metadata_property,
};

static void init_defaults(struct impl *impl)
{
	struct settings *s = &impl->context->settings;

	pw_impl_metadata_set_propertyf(impl->metadata,
			PW_ID_CORE, "log.level", "", "%d", s->log_level);
	pw_impl_metadata_set_propertyf(impl->metadata,
			PW_ID_CORE, "clock.min-quantum", "", "%d", s->clock_min_quantum);
	pw_impl_metadata_set_propertyf(impl->metadata,
			PW_ID_CORE, "clock.max-quantum", "", "%d", s->clock_max_quantum);
	pw_impl_metadata_set_propertyf(impl->metadata,
			PW_ID_CORE, "clock.force-quantum", "", "%d", s->clock_force_quantum);
	pw_impl_metadata_set_propertyf(impl->metadata,
			PW_ID_CORE, "clock.force-rate", "", "%d", s->clock_force_rate);
}

int pw_settings_init(struct pw_context *context)
{
	struct impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -errno;

	impl->context = context;
	impl->metadata = pw_context_create_metadata(context, "settings", NULL, 0);
	if (impl->metadata == NULL)
		goto error_free;

	init_defaults(impl);

	pw_impl_metadata_add_listener(impl->metadata,
			&impl->metadata_listener,
			&metadata_events, impl);

	pw_impl_metadata_register(impl->metadata, NULL);

	context->settings_impl = impl;

	return 0;

error_free:
	free(impl);
	return -errno;
}

void pw_settings_clean(struct pw_context *context)
{
	struct impl *impl = context->settings_impl;

	if (impl == NULL)
		return;

	context->settings_impl = NULL;
	if (impl->metadata != NULL)
		pw_impl_metadata_destroy(impl->metadata);
	free(impl);
}
