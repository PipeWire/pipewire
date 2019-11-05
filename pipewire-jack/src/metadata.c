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
#include <jack/uuid.h>

#include <pipewire/pipewire.h>
#include <extensions/metadata.h>

static struct pw_properties * get_properties(void)
{
	if (globals.properties == NULL) {
		globals.properties = pw_properties_new(NULL, NULL);
	}
	return globals.properties;
}

static void make_key(char *dst, jack_uuid_t subject, const char *key, int keylen)
{
	int len;
	jack_uuid_unparse (subject, dst);
	len = strlen(dst);
	dst[len] = '@';
	memcpy(&dst[len+1], key, keylen+1);
}

SPA_EXPORT
int jack_set_property(jack_client_t*client,
		      jack_uuid_t subject,
		      const char* key,
		      const char* value,
		      const char* type)
{
	struct client *c = (struct client *) client;
	uint32_t id;

	id = jack_uuid_to_index(subject);

	pw_log_debug("set id:%u (%lu) '%s' to '%s@%s'", id, subject, key, value, type);
	pw_metadata_set_property(c->metadata->proxy,
			id, key, type, value);
	return 0;
}

SPA_EXPORT
int jack_get_property(jack_uuid_t subject,
		      const char* key,
		      char**      value,
		      char**      type)
{
	int keylen = strlen(key);
	char *dst = alloca(JACK_UUID_STRING_SIZE + keylen);
	struct pw_properties * props = get_properties();
	const char *str, *at;

	make_key(dst, subject, key, keylen);

	if ((str = pw_properties_get(props, dst)) == NULL) {
		pw_log_warn("no property '%s'", dst);
		return -1;
	}

	at = strrchr(str, '@');
	if (at == NULL) {
		pw_log_warn("property '%s' invalid value '%s'", dst, str);
		return -1;
	}

	*value = strndup(str, at - str);
	*type = strdup(at + 1);

	pw_log_debug("got '%s' with value:'%s' type:'%s'", dst, *value, *type);

	return 0;
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
	int keylen = strlen(key);
	char *dst = alloca(JACK_UUID_STRING_SIZE + keylen);
	struct pw_properties * props = get_properties();

	make_key(dst, subject, key, keylen);

	pw_properties_set(props, dst, NULL);
	pw_log_debug("removed %s", dst);

	return 0;
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
