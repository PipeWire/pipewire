/* Spa libcamera support */
/* SPDX-FileCopyrightText: Copyright © 2020 collabora */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

#include "libcamera.h"

struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.libcamera");
struct spa_log_topic *libcamera_log_topic = &log_topic;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
			uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_libcamera_manager_factory;
		break;
	case 1:
		*factory = &spa_libcamera_device_factory;
		break;
	case 2:
		*factory = &spa_libcamera_source_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
