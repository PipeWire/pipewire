/* Spa Video Test Source plugin
 * Copyright (C) 2016 Axis Communications AB
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

#include <spa/plugin.h>
#include <spa/node.h>

#define MAX_FACTORIES	16

static const struct spa_handle_factory *factories[MAX_FACTORIES];
static int n_factories;

void
spa_handle_factory_register(const struct spa_handle_factory *factory)
{
	if (n_factories < MAX_FACTORIES)
		factories[n_factories++] = factory;
	else
		fprintf(stderr, "too many factories\n");
}

int
spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (index >= n_factories)
		return SPA_RESULT_ENUM_END;

	*factory = factories[index];
	return SPA_RESULT_OK;
}
