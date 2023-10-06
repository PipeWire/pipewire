/* Spa V4l2 support */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

#include "v4l2.h"

extern const struct spa_handle_factory spa_v4l2_source_factory;
extern const struct spa_handle_factory spa_v4l2_udev_factory;
extern const struct spa_handle_factory spa_v4l2_device_factory;

struct spa_log_topic v4l2_log_topic = SPA_LOG_TOPIC(0, "spa.v4l2");

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
			uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_v4l2_source_factory;
		break;
	case 1:
		*factory = &spa_v4l2_udev_factory;
		break;
	case 2:
		*factory = &spa_v4l2_device_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
