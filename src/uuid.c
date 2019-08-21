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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#include <jack/uuid.h>

#include <pipewire/pipewire.h>

SPA_EXPORT
jack_uuid_t jack_client_uuid_generate ()
{
	static uint32_t uuid_cnt = 0;
	jack_uuid_t uuid = 0x2; /* JackUUIDClient */;
	uuid = (uuid << 32) | ++uuid_cnt;
	pw_log_debug("uuid %"PRIu64, uuid);
	return uuid;
}

SPA_EXPORT
jack_uuid_t jack_port_uuid_generate (uint32_t port_id)
{
	jack_uuid_t uuid = 0x1; /* JackUUIDPort */
	uuid = (uuid << 32) | (port_id + 1);
	pw_log_debug("uuid %d -> %"PRIu64, port_id, uuid);
	return uuid;
}

SPA_EXPORT
uint32_t jack_uuid_to_index (jack_uuid_t id)
{
	return (id & 0xffff) - 1;
}

SPA_EXPORT
int  jack_uuid_compare (jack_uuid_t id1, jack_uuid_t id2)
{
	if (id1 == id2)
		return 0;
	if (id1 < id2)
		return -1;
	return 1;
}

SPA_EXPORT
void jack_uuid_copy (jack_uuid_t* dst, jack_uuid_t src)
{
	*dst = src;
}

SPA_EXPORT
void jack_uuid_clear (jack_uuid_t *id)
{
	*id = 0;
}

SPA_EXPORT
int  jack_uuid_parse (const char *buf, jack_uuid_t *id)
{
	if (sscanf (buf, "%" PRIu64, id) == 1) {
		if (*id < (0x1LL << 32)) {
			/* has not type bits set - not legal */
			return -1;
		}
		return 0;
	}
	return -1;
}

SPA_EXPORT
void jack_uuid_unparse (jack_uuid_t id, char buf[JACK_UUID_STRING_SIZE])
{
	snprintf (buf, JACK_UUID_STRING_SIZE, "%" PRIu64, id);
}

SPA_EXPORT
int  jack_uuid_empty (jack_uuid_t id)
{
	return id == 0;
}
