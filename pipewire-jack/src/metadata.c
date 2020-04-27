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

static struct pw_array * get_descriptions(void)
{
	if (globals.descriptions.extend == 0) {
		pw_array_init(&globals.descriptions, 16);
	}
	return &globals.descriptions;
}

static jack_description_t *find_description(jack_uuid_t subject)
{
	struct pw_array *descriptions = get_descriptions();
	jack_description_t *desc;

	pw_array_for_each(desc, descriptions) {
		if (jack_uuid_compare(desc->subject, subject) == 0)
			return desc;
	}
	return NULL;
}

static void set_property(jack_property_t *prop, const char *key, const char *value, const char *type)
{
	prop->key = strdup(key);
	prop->data = strdup(value);
	prop->type = strdup(type);
}

static jack_property_t *copy_properties(jack_property_t *src, uint32_t cnt)
{
	jack_property_t *dst;
	uint32_t i;
	dst = malloc(sizeof(jack_property_t) * cnt);
	if (dst != NULL) {
		for (i = 0; i < cnt; i++)
			set_property(&dst[i], src[i].key, src[i].data, src[i].type);
	}
	return dst;
}

static int copy_description(jack_description_t *dst, jack_description_t *src)
{
	dst->properties = copy_properties(src->properties, src->property_cnt);
	if (dst->properties == NULL)
		return -errno;
	jack_uuid_copy(&dst->subject, src->subject);
	dst->property_cnt = src->property_cnt;
	dst->property_size = src->property_size;
	return dst->property_cnt;
}

static jack_description_t *add_description(jack_uuid_t subject)
{
	struct pw_array *descriptions = get_descriptions();
	jack_description_t *desc;
	desc = pw_array_add(descriptions, sizeof(*desc));
	if (desc != NULL) {
		spa_zero(*desc);
		jack_uuid_copy(&desc->subject, subject);
	}
	return desc;
}

static void remove_description(jack_description_t *desc)
{
	struct pw_array *descriptions = get_descriptions();
	jack_free_description(desc, false);
	pw_array_remove(descriptions, desc);
}

static jack_property_t *find_property(jack_description_t *desc, const char *key)
{
	uint32_t i;
	for (i = 0; i < desc->property_cnt; i++) {
		jack_property_t *prop = &desc->properties[i];
		if (strcmp(prop->key, key) == 0)
			return prop;
	}
	return NULL;
}

static jack_property_t *add_property(jack_description_t *desc, const char *key,
		const char *value, const char *type)
{
	jack_property_t *prop;

	if (desc->property_cnt == desc->property_size) {
		desc->property_size = desc->property_size > 0 ? desc->property_size * 2 : 8;
		desc->properties = realloc(desc->properties, sizeof(*prop) * desc->property_size);
	}
	prop = &desc->properties[desc->property_cnt++];
	set_property(prop, key, value, type);
	return prop;
}

static void clear_property(jack_property_t *prop)
{
	free((char*)prop->key);
	free((char*)prop->data);
	free((char*)prop->type);
}

static void remove_property(jack_description_t *desc, jack_property_t *prop)
{
	clear_property(prop);
	desc->property_cnt--;
        memmove(desc->properties, SPA_MEMBER(prop, sizeof(*prop), void),
                SPA_PTRDIFF(SPA_MEMBER(desc->properties, sizeof(*prop) * desc->property_cnt, void),
			prop));

	if (desc->property_cnt == 0)
		remove_description(desc);
}

static void change_property(jack_property_t *prop, const char *value, const char *type)
{
	free((char*)prop->data);
	prop->data = strdup(value);
	free((char*)prop->type);
	prop->type = strdup(type);
}

static int update_property(struct client *c,
		      jack_uuid_t subject,
		      const char* key,
		      const char* type,
		      const char* value)
{
	jack_property_change_t change;
	jack_description_t *desc;

	desc = find_description(subject);

	if (key == NULL) {
		if (desc == NULL)
			return 0;
		remove_description(desc);
		change = PropertyDeleted;
	} else {
		jack_property_t *prop;

		prop = desc ? find_property(desc, key) : NULL;

		if (value == NULL || type == NULL) {
			if (prop == NULL)
				return 0;
			remove_property(desc, prop);
			change = PropertyDeleted;
		} else if (prop == NULL) {
			if (desc == NULL)
				desc = add_description(subject);
			prop = add_property(desc, key, value, type);
			change = PropertyCreated;
		} else {
			change_property(prop, value, type);
			change = PropertyChanged;
		}
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
	jack_description_t *desc;
	jack_property_t *prop;

	desc = find_description(subject);
	if (desc == NULL)
		return -1;

	prop = find_property(desc, key);
	if (prop == NULL)
		return -1;

	*value = strdup(prop->data);
	*type = strdup(prop->type);

	pw_log_debug("subject:%"PRIu64" key:'%s' value:'%s' type:'%s'",
			subject, key, *value, *type);

	return 0;
}

SPA_EXPORT
void jack_free_description (jack_description_t* desc, int free_description_itself)
{
	uint32_t n;

	for (n = 0; n < desc->property_cnt; ++n)
		clear_property(&desc->properties[n]);
	free(desc->properties);
	if (free_description_itself)
		free(desc);
}

SPA_EXPORT
int jack_get_properties (jack_uuid_t         subject,
			 jack_description_t* desc)
{
	jack_description_t *d;

	spa_return_val_if_fail(desc != NULL, -EINVAL);

	d = find_description(subject);
	if (d == NULL)
		return -1;
	return copy_description(desc, d);
}

SPA_EXPORT
int jack_get_all_properties (jack_description_t** result)
{
	uint32_t i;
	jack_description_t *dst, *src;
	struct pw_array *descriptions = get_descriptions();
	uint32_t len = pw_array_get_len(descriptions, jack_description_t);
	src = descriptions->data;

	dst = malloc(descriptions->size);
	for (i = 0; i < len; i++)
		copy_description(&dst[i], &src[i]);

	*result = dst;
	return len;
}

SPA_EXPORT
int jack_remove_property (jack_client_t* client, jack_uuid_t subject, const char* key)
{
	struct client *c = (struct client *) client;
	uint32_t id;

	spa_return_val_if_fail(c != NULL, -EINVAL);
	spa_return_val_if_fail(key != NULL, -EINVAL);

	if (c->metadata == NULL)
		return -1;

	id = jack_uuid_to_index(subject);

	pw_log_info("remove id:%u (%lu) '%s'", id, subject, key);
	pw_metadata_set_property(c->metadata->proxy,
			id, key, NULL, NULL);

	return 0;
}

SPA_EXPORT
int jack_remove_properties (jack_client_t* client, jack_uuid_t subject)
{
	struct client *c = (struct client *) client;
	uint32_t id;

	spa_return_val_if_fail(c != NULL, -EINVAL);

	if (c->metadata == NULL)
		return -1;

	id = jack_uuid_to_index(subject);

	pw_log_info("remove id:%u (%lu)", id, subject);
	pw_metadata_set_property(c->metadata->proxy,
			id, NULL, NULL, NULL);

	return 0;
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

#define JACK_METADATA_PREFIX "http://jackaudio.org/metadata/"
SPA_EXPORT const char* JACK_METADATA_CONNECTED   = JACK_METADATA_PREFIX "connected";
SPA_EXPORT const char* JACK_METADATA_EVENT_TYPES = JACK_METADATA_PREFIX "event-types";
SPA_EXPORT const char* JACK_METADATA_HARDWARE    = JACK_METADATA_PREFIX "hardware";
SPA_EXPORT const char* JACK_METADATA_ICON_LARGE  = JACK_METADATA_PREFIX "icon-large";
SPA_EXPORT const char* JACK_METADATA_ICON_NAME   = JACK_METADATA_PREFIX "icon-name";
SPA_EXPORT const char* JACK_METADATA_ICON_SMALL  = JACK_METADATA_PREFIX "icon-small";
SPA_EXPORT const char* JACK_METADATA_ORDER       = JACK_METADATA_PREFIX "order";
SPA_EXPORT const char* JACK_METADATA_PORT_GROUP  = JACK_METADATA_PREFIX "port-group";
SPA_EXPORT const char* JACK_METADATA_PRETTY_NAME = JACK_METADATA_PREFIX "pretty-name";
SPA_EXPORT const char* JACK_METADATA_SIGNAL_TYPE = JACK_METADATA_PREFIX "signal-type";
#undef JACK_METADATA_PREFIX
