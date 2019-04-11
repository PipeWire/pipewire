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

#include <jack/metadata.h>

#include <pipewire/pipewire.h>

SPA_EXPORT
int jack_set_property(jack_client_t*client,
		      jack_uuid_t subject,
		      const char* key,
		      const char* value,
		      const char* type)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_get_property(jack_uuid_t subject,
		      const char* key,
		      char**      value,
		      char**      type)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
void jack_free_description (jack_description_t* desc, int free_description_itself)
{
	pw_log_warn("not implemented");
}

SPA_EXPORT
int jack_get_properties (jack_uuid_t         subject,
			 jack_description_t* desc)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_get_all_properties (jack_description_t** descs)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_remove_property (jack_client_t* client, jack_uuid_t subject, const char* key)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_remove_properties (jack_client_t* client, jack_uuid_t subject)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_remove_all_properties (jack_client_t* client)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
int jack_set_property_change_callback (jack_client_t*             client,
                                       JackPropertyChangeCallback callback,
                                       void*                      arg)
{
	pw_log_warn("not implemented");
	return -1;
}

SPA_EXPORT
const char* JACK_METADATA_PRETTY_NAME = "http://jackaudio.org/metadata/pretty-name";
SPA_EXPORT
const char* JACK_METADATA_HARDWARE = "http://jackaudio.org/metadata/hardware";
SPA_EXPORT
const char* JACK_METADATA_CONNECTED = "http://jackaudio.org/metadata/connected";
SPA_EXPORT
const char* JACK_METADATA_PORT_GROUP = "http://jackaudio.org/metadata/port-group";
SPA_EXPORT
const char* JACK_METADATA_ICON_SMALL = "http://jackaudio.org/metadata/icon-small";
SPA_EXPORT
const char* JACK_METADATA_ICON_LARGE = "http://jackaudio.org/metadata/icon-large";
