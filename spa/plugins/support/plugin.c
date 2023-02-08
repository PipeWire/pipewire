/* Spa Support plugin */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>

#include <spa/support/plugin.h>

extern const struct spa_handle_factory spa_support_logger_factory;
extern const struct spa_handle_factory spa_support_system_factory;
extern const struct spa_handle_factory spa_support_cpu_factory;
extern const struct spa_handle_factory spa_support_loop_factory;
extern const struct spa_handle_factory spa_support_node_driver_factory;
extern const struct spa_handle_factory spa_support_null_audio_sink_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_support_logger_factory;
		break;
	case 1:
		*factory = &spa_support_system_factory;
		break;
	case 2:
		*factory = &spa_support_cpu_factory;
		break;
	case 3:
		*factory = &spa_support_loop_factory;
		break;
	case 4:
		*factory = &spa_support_node_driver_factory;
		break;
	case 5:
		*factory = &spa_support_null_audio_sink_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
