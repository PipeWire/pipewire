/* Metadata API
 *
 * Copyright © 2019 Wim Taymans
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

#include <extensions/metadata.h>

#define pw_metadata_emit(hooks,method,version,...)			\
	spa_hook_list_call_simple(hooks, struct pw_metadata_events,	\
				method, version, ##__VA_ARGS__)

#define pw_metadata_emit_property(hooks,...)	pw_metadata_emit(hooks,property, 0, ##__VA_ARGS__)

struct item {
	uint32_t subject;
	char *key;
	char *type;
	char *value;
};

static void clear_item(struct item *item)
{
	free(item->key);
	free(item->type);
	free(item->value);
	spa_zero(*item);
}


static void set_item(struct item *item, uint32_t subject, const char *key, const char *type, const char *value)
{
	item->subject = subject;
	item->key = strdup(key);
	item->type = strdup(type);
	item->value = strdup(value);
}

struct sm_metadata {
	struct pw_metadata impl;

	struct spa_hook_list hooks;

	struct pw_properties *properties;

	struct pw_array metadata;
};

static void emit_properties(struct sm_metadata *this, const struct spa_dict *dict)
{
	struct item *item;
	pw_array_for_each(item, &this->metadata) {
		pw_metadata_emit_property(&this->hooks,
				item->subject,
				item->key,
				item->type,
				item->value);
	}
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct pw_metadata_events *events,
		void *data)
{
	struct sm_metadata *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_properties(this, &this->properties->dict);

	spa_hook_list_join(&this->hooks, &save);

        return 0;
}

static struct item *find_item(struct sm_metadata *this, uint32_t subject, const char *key)
{
	struct item *item;

	pw_array_for_each(item, &this->metadata) {
		if (item->subject == subject && !strcmp(item->key, key))
			return item;
	}
	return NULL;
}

static void clear_items(struct sm_metadata *this)
{
	struct item *item;

	pw_array_for_each(item, &this->metadata)
		clear_item(item);

	pw_array_reset(&this->metadata);
}

static int impl_set_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct sm_metadata *this = object;
	struct item *item = NULL;

	if (key == NULL)
		return -EINVAL;

	item = find_item(this, subject, key);
	if (item == NULL) {
		if (value == NULL)
			return 0;
		item = pw_array_add(&this->metadata, sizeof(*item));
		if (item == NULL)
			return -errno;
	} else {
		clear_item(item);

	}
	if (value != NULL) {
		if (type == NULL)
			type = "string";
		set_item(item, subject, key, type, value);
		pw_log_debug(NAME" %p: add id:%d key:%s type:%s value:%s", this,
				subject, key, type, value);
	} else {
		type = NULL;
		pw_array_remove(&this->metadata, item);
		pw_log_debug(NAME" %p: remove id:%d key:%s", this, subject, key);
	}


	pw_metadata_emit_property(&this->hooks,
				subject, key, type, value);
	return 0;
}

static int impl_clear(void *object)
{
	struct sm_metadata *this = object;
	clear_items(this);
	return 0;
}

struct pw_metadata_methods impl_metadata = {
	PW_VERSION_METADATA_METHODS,
	.add_listener = impl_add_listener,
	.set_property = impl_set_property,
	.clear = impl_clear,
};

struct sm_metadata *
sm_metadata_new(struct pw_properties *props)
{
	struct sm_metadata *md;

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	md = calloc(1, sizeof(*md));
	md->properties = props;
	pw_array_init(&md->metadata, 4096);

	md->impl.iface = SPA_INTERFACE_INIT(
			PW_TYPE_INTERFACE_Metadata,
			PW_VERSION_METADATA,
			&impl_metadata, md);
        spa_hook_list_init(&md->hooks);

	return md;
}

void sm_metadata_destroy(struct sm_metadata *this)
{
	clear_items(this);
	pw_array_clear(&this->metadata);
	pw_properties_free(this->properties);
	free(this);
}
