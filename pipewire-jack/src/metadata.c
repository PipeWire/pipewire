/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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
	jack_uuid_unparse(subject, dst);
	len = strlen(dst);
	dst[len] = '@';
	memcpy(&dst[len+1], key, keylen+1);
}

static int update_property(struct client *c,
		      jack_uuid_t subject,
		      const char* key,
		      const char* value,
		      const char* type)
{
	int keylen = strlen(key);
	char *dst = alloca(JACK_UUID_STRING_SIZE + keylen);
	struct pw_properties * props = get_properties();
	jack_property_change_t change;

	make_key(dst, subject, key, keylen);

	if (value == NULL || type == NULL) {
		pw_properties_setf(props, dst, NULL);
		change = PropertyDeleted;
	} else {
		change = PropertyCreated;
		if (pw_properties_get(props, dst) != NULL)
			change = PropertyChanged;

		pw_properties_setf(props, dst, "%s@%s", value, type);
	}

	if (c->property_callback)
		c->property_callback(subject, key, change, c->property_arg);

	return 0;
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

	spa_return_val_if_fail(c != NULL, -EINVAL);
	spa_return_val_if_fail(key != NULL, -EINVAL);
	spa_return_val_if_fail(value != NULL, -EINVAL);

	if (c->metadata == NULL)
		return -1;

	id = jack_uuid_to_index(subject);

	if (type == NULL)
		type = "";

	pw_log_info("set id:%u (%lu) '%s' to '%s@%s'", id, subject, key, value, type);
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
	int keylen;
	char *dst;
	struct pw_properties * props = get_properties();
	const char *str, *at;

	spa_return_val_if_fail(props != NULL, -EIO);
	spa_return_val_if_fail(key != NULL, -EINVAL);
	spa_return_val_if_fail(value != NULL, -EINVAL);
	spa_return_val_if_fail(type != NULL, -EINVAL);

	keylen = strlen(key);
	dst = alloca(JACK_UUID_STRING_SIZE + keylen);
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

	pw_log_info("got '%s' with value:'%s' type:'%s'", dst, *value, *type);

	return 0;
}

SPA_EXPORT
void jack_free_description (jack_description_t* desc, int free_description_itself)
{
	uint32_t n;

	for (n = 0; n < desc->property_cnt; ++n) {
		free((char*)desc->properties[n].key);
		free((char*)desc->properties[n].data);
		if (desc->properties[n].type)
			free((char*)desc->properties[n].type);
	}

	free(desc->properties);

	if (free_description_itself)
		free(desc);
}

SPA_EXPORT
int jack_get_properties (jack_uuid_t         subject,
			 jack_description_t* desc)
{
	const struct spa_dict_item *item;
	struct pw_properties * props = get_properties();
	char dst[JACK_UUID_STRING_SIZE+2], *at;
	uint32_t props_size, cnt;
	int len;

	spa_return_val_if_fail(props != NULL, -EIO);
	spa_return_val_if_fail(desc != NULL, -EINVAL);

        jack_uuid_copy(&desc->subject, subject);
	desc->properties = NULL;
	cnt = props_size = 0;

	jack_uuid_unparse(subject, dst);
	len = strlen(dst);
	dst[len] = '@';
	dst[len+1] = 0;

	pw_log_debug("keys for: %s", dst);

	spa_dict_for_each(item, &props->dict) {
		jack_property_t* prop;

		pw_log_debug("keys %s <-> %s", item->key, dst);

		if (strstr(item->key, dst) != item->key)
			continue;

		if (cnt == props_size) {
		        props_size = props_size > 0 ? props_size * 2 : 8;
			desc->properties = realloc(desc->properties, sizeof(jack_property_t) * props_size);
		}
		prop = &desc->properties[cnt];

		pw_log_debug("match %s", item->value);
		at = strrchr(item->value, '@');
		if (at == NULL)
			continue;

		prop->key = strdup(item->key + len + 1);
		prop->data = strndup(item->value, at - item->value);
		prop->type = strdup(at + 1);
		cnt++;
        }
	desc->property_cnt = cnt;
	return cnt;
}

SPA_EXPORT
int jack_get_all_properties (jack_description_t** descriptions)
{
	const struct spa_dict_item *item;
	struct pw_properties * props = get_properties();
	char *at;
	jack_description_t *desc, *cdesc;
	uint32_t dsize, dcnt, n, len;
	jack_uuid_t uuid;

	spa_return_val_if_fail(props != NULL, -EIO);
	spa_return_val_if_fail(descriptions != NULL, -EINVAL);

	dsize = dcnt = 0;
	desc = NULL;

	spa_dict_for_each(item, &props->dict) {
		jack_property_t* prop;

		at = strrchr(item->key, '@');
		if (at == NULL)
			continue;

		len = at - item->key;

		jack_uuid_parse(item->key, &uuid);

		at = strrchr(item->value, '@');
		if (at == NULL)
			continue;

		for (n = 0; n < dcnt; n++) {
			if (jack_uuid_compare(uuid, desc[n].subject) == 0)
				break;
		}
		if (n == dcnt) {
			if (dcnt == dsize) {
				dsize = dsize > 0 ? dsize * 2 : 8;
				desc = realloc (desc, sizeof(jack_description_t) * dsize);
			}
			desc[n].property_size = 0;
			desc[n].property_cnt = 0;
			desc[n].properties = NULL;

			jack_uuid_copy (&desc[n].subject, uuid);
			dcnt++;
		}
		cdesc = &desc[n];

		if (cdesc->property_cnt == cdesc->property_size) {
			cdesc->property_size = cdesc->property_size > 0 ? cdesc->property_size * 2 : 8;
			cdesc->properties = realloc (cdesc->properties,
					sizeof(jack_property_t) * cdesc->property_size);
		}

		prop = &cdesc->properties[cdesc->property_cnt++];
		prop->key = strdup(item->key + len + 1);
		prop->data = strndup(item->value, at - item->value);
		prop->type = strdup(at + 1);
        }
	*descriptions = desc;
	return dcnt;
}

SPA_EXPORT
int jack_remove_property (jack_client_t* client, jack_uuid_t subject, const char* key)
{
	int keylen;
	char *dst;
	struct pw_properties * props = get_properties();

	spa_return_val_if_fail(client != NULL, -EINVAL);
	spa_return_val_if_fail(key != NULL, -EINVAL);

	keylen = strlen(key);
	dst = alloca(JACK_UUID_STRING_SIZE + keylen);
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
	struct client *c = (struct client *) client;

	spa_return_val_if_fail(c != NULL, -EINVAL);

	pw_metadata_clear(c->metadata->proxy);
	return 0;
}

SPA_EXPORT
int jack_set_property_change_callback (jack_client_t*             client,
                                       JackPropertyChangeCallback callback,
                                       void*                      arg)
{
	struct client *c = (struct client *) client;

	spa_return_val_if_fail(c != NULL, -EINVAL);

	c->property_callback = callback;
	c->property_arg = arg;
	return 0;
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
