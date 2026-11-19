/* Spa sndio plugin */
/* SPDX-FileCopyrightText: Copyright © 2026 Antoine Jacoutot <ajacoutot@openbsd.org> */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

extern const struct spa_handle_factory spa_sndio_sink_factory;
extern const struct spa_handle_factory spa_sndio_source_factory;
extern const struct spa_handle_factory spa_sndio_device_factory;

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_sndio_sink_factory;
		break;
	case 1:
		*factory = &spa_sndio_source_factory;
		break;
	case 2:
		*factory = &spa_sndio_device_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
