/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <errno.h>

#include <spa/utils/defs.h>
#include <pulse/direction.h>

#define pa_init_i18n()
#define _(String)	(String)

SPA_EXPORT
int pa_direction_valid(pa_direction_t direction)
{
	if (direction != PA_DIRECTION_INPUT
	    && direction != PA_DIRECTION_OUTPUT
	    && direction != (PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT))
        return 0;
    return 1;
}

SPA_EXPORT
const char *pa_direction_to_string(pa_direction_t direction) {
	pa_init_i18n();

	if (direction == PA_DIRECTION_INPUT)
		return _("input");
	if (direction == PA_DIRECTION_OUTPUT)
		return _("output");
	if (direction == (PA_DIRECTION_INPUT | PA_DIRECTION_OUTPUT))
		return _("bidirectional");

	return _("invalid");
}
