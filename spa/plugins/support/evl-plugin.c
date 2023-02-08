/* Spa Support plugin */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>

#include <spa/support/plugin.h>

extern const struct spa_handle_factory spa_support_evl_system_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_support_evl_system_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
