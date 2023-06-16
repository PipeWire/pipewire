/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdarg.h>

#include <spa/utils/ansi.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "pipewire/array.h"
#include "pipewire/log.h"
#include "pipewire/utils.h"
#include "pipewire/properties.h"

PW_LOG_TOPIC_EXTERN(log_properties);
#define PW_LOG_TOPIC_DEFAULT log_properties

/** \cond */
struct properties {
	struct pw_properties this;

	struct pw_array items;
};
/** \endcond */

static int add_func(struct pw_properties *this, char *key, char *value)
{
	struct spa_dict_item *item;
	struct properties *impl = SPA_CONTAINER_OF(this, struct properties, this);

	item = pw_array_add(&impl->items, sizeof(struct spa_dict_item));
	if (item == NULL) {
		free(key);
		free(value);
		return -errno;
	}

	item->key = key;
	item->value = value;

	this->dict.items = impl->items.data;
	this->dict.n_items++;
	return 0;
}

static void clear_item(struct spa_dict_item *item)
{
	free((char *) item->key);
	free((char *) item->value);
}

static int find_index(const struct pw_properties *this, const char *key)
{
	const struct spa_dict_item *item;
	item = spa_dict_lookup_item(&this->dict, key);
	if (item == NULL)
		return -1;
	return item - this->dict.items;
}

static struct properties *properties_new(int prealloc)
{
	struct properties *impl;

	impl = calloc(1, sizeof(struct properties));
	if (impl == NULL)
		return NULL;

	pw_array_init(&impl->items, 16);
	pw_array_ensure_size(&impl->items, sizeof(struct spa_dict_item) * prealloc);

	return impl;
}

/** Make a new properties object
 *
 * \param key a first key
 * \param ... value and more keys NULL terminated
 * \return a newly allocated properties object
 */
SPA_EXPORT
struct pw_properties *pw_properties_new(const char *key, ...)
{
	struct properties *impl;
	va_list varargs;
	const char *value;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	va_start(varargs, key);
	while (key != NULL) {
		value = va_arg(varargs, char *);
		if (value && key[0])
			add_func(&impl->this, strdup(key), strdup(value));
		key = va_arg(varargs, char *);
	}
	va_end(varargs);

	return &impl->this;
}

/** Make a new properties object from the given dictionary
 *
 * \param dict a dictionary. keys and values are copied
 * \return a new properties object
 */
SPA_EXPORT
struct pw_properties *pw_properties_new_dict(const struct spa_dict *dict)
{
	uint32_t i;
	struct properties *impl;

	impl = properties_new(SPA_ROUND_UP_N(dict->n_items, 16));
	if (impl == NULL)
		return NULL;

	for (i = 0; i < dict->n_items; i++) {
		const struct spa_dict_item *it = &dict->items[i];
		if (it->key != NULL && it->key[0] && it->value != NULL)
			add_func(&impl->this, strdup(it->key),
				 strdup(it->value));
	}

	return &impl->this;
}

/** Update the properties from the given string, overwriting any
 * existing keys with the new values from \a str.
 *
 * \a str should be a whitespace separated list of key=value
 * strings or a json object, see pw_properties_new_string().
 *
 * \return The number of properties added or updated
 */
SPA_EXPORT
int pw_properties_update_string(struct pw_properties *props, const char *str, size_t size)
{
	struct properties *impl = SPA_CONTAINER_OF(props, struct properties, this);
	struct spa_json it[2];
	char key[1024], *val;
	int count = 0;

	spa_json_init(&it[0], str, size);
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], str, size);

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		int len;
		const char *value;

		if ((len = spa_json_next(&it[1], &value)) <= 0)
			break;

		if (spa_json_is_null(value, len))
			val = NULL;
		else {
			if (spa_json_is_container(value, len))
				len = spa_json_container_len(&it[1], value, len);

			if ((val = malloc(len+1)) != NULL)
				spa_json_parse_stringn(value, len, val, len+1);
		}
		count += pw_properties_set(&impl->this, key, val);
		free(val);
	}
	return count;
}

/** Make a new properties object from the given str
 *
 * \a object should be a whitespace separated list of key=value
 * strings or a json object.
 *
 * \param object a property description
 * \return a new properties object
 */
SPA_EXPORT
struct pw_properties *
pw_properties_new_string(const char *object)
{
	struct properties *impl;
	int res;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	if ((res = pw_properties_update_string(&impl->this, object, strlen(object))) < 0)
		goto error;

	return &impl->this;
error:
	pw_properties_free(&impl->this);
	errno = -res;
	return NULL;
}

/** Copy a properties object
 *
 * \param properties properties to copy
 * \return a new properties object
 */
SPA_EXPORT
struct pw_properties *pw_properties_copy(const struct pw_properties *properties)
{
	return pw_properties_new_dict(&properties->dict);
}

/** Copy multiple keys from one property to another
 *
 * \param props properties to copy to
 * \param dict properties to copy from
 * \param keys a NULL terminated list of keys to copy
 * \return the number of keys changed in \a dest
 */
SPA_EXPORT
int pw_properties_update_keys(struct pw_properties *props,
		const struct spa_dict *dict, const char * const keys[])
{
	int i, changed = 0;
	const char *str;

	for (i = 0; keys[i]; i++) {
		if ((str = spa_dict_lookup(dict, keys[i])) != NULL)
			changed += pw_properties_set(props, keys[i], str);
	}
	return changed;
}

static bool has_key(const char * const keys[], const char *key)
{
	int i;
	for (i = 0; keys[i]; i++) {
		if (spa_streq(keys[i], key))
			return true;
	}
	return false;
}

SPA_EXPORT
int pw_properties_update_ignore(struct pw_properties *props,
		const struct spa_dict *dict, const char * const ignore[])
{
	const struct spa_dict_item *it;
	int changed = 0;

	spa_dict_for_each(it, dict) {
		if (ignore == NULL || !has_key(ignore, it->key))
			changed += pw_properties_set(props, it->key, it->value);
	}
	return changed;
}

/** Clear a properties object
 *
 * \param properties properties to clear
 */
SPA_EXPORT
void pw_properties_clear(struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	struct spa_dict_item *item;

	pw_array_for_each(item, &impl->items)
		clear_item(item);
	pw_array_reset(&impl->items);
	properties->dict.n_items = 0;
}

/** Update properties
 *
 * \param props properties to update
 * \param dict new properties
 * \return the number of changed properties
 *
 * The properties in \a props are updated with \a dict. Keys in \a dict
 * with NULL values are removed from \a props.
 */
SPA_EXPORT
int pw_properties_update(struct pw_properties *props,
		         const struct spa_dict *dict)
{
	const struct spa_dict_item *it;
	int changed = 0;

	spa_dict_for_each(it, dict)
		changed += pw_properties_set(props, it->key, it->value);

	return changed;
}

/** Add properties
 *
 * \param props properties to add
 * \param dict new properties
 * \return the number of added properties
 *
 * The properties from \a dict that are not yet in \a props are added.
 */
SPA_EXPORT
int pw_properties_add(struct pw_properties *props,
		         const struct spa_dict *dict)
{
	uint32_t i;
	int added = 0;

	for (i = 0; i < dict->n_items; i++) {
		if (pw_properties_get(props, dict->items[i].key) == NULL)
			added += pw_properties_set(props, dict->items[i].key, dict->items[i].value);
	}
	return added;
}

/** Add keys
 *
 * \param props properties to add
 * \param dict new properties
 * \param keys a NULL terminated list of keys to add
 * \return the number of added properties
 *
 * The properties with \a keys from \a dict that are not yet
 * in \a props are added.
 */
SPA_EXPORT
int pw_properties_add_keys(struct pw_properties *props,
		const struct spa_dict *dict, const char * const keys[])
{
	uint32_t i;
	int added = 0;
	const char *str;

	for (i = 0; keys[i]; i++) {
		if ((str = spa_dict_lookup(dict, keys[i])) == NULL)
			continue;
		if (pw_properties_get(props, keys[i]) == NULL)
			added += pw_properties_set(props, keys[i], str);
	}
	return added;
}

/** Free a properties object
 *
 * \param properties the properties to free
 */
SPA_EXPORT
void pw_properties_free(struct pw_properties *properties)
{
	struct properties *impl;

	if (properties == NULL)
		return;

	impl = SPA_CONTAINER_OF(properties, struct properties, this);
	pw_properties_clear(properties);
	pw_array_clear(&impl->items);
	free(impl);
}

static int do_replace(struct pw_properties *properties, const char *key, char *value, bool copy)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index;

	if (key == NULL || key[0] == 0)
		goto exit_noupdate;

	index = find_index(properties, key);

	if (index == -1) {
		if (value == NULL)
			return 0;
		add_func(properties, strdup(key), copy ? strdup(value) : value);
		SPA_FLAG_CLEAR(properties->dict.flags, SPA_DICT_FLAG_SORTED);
	} else {
		struct spa_dict_item *item =
		    pw_array_get_unchecked(&impl->items, index, struct spa_dict_item);

		if (value && spa_streq(item->value, value))
			goto exit_noupdate;

		if (value == NULL) {
			struct spa_dict_item *last = pw_array_get_unchecked(&impl->items,
						     pw_array_get_len(&impl->items, struct spa_dict_item) - 1,
						     struct spa_dict_item);
			clear_item(item);
			item->key = last->key;
			item->value = last->value;
			impl->items.size -= sizeof(struct spa_dict_item);
			properties->dict.n_items--;
			SPA_FLAG_CLEAR(properties->dict.flags, SPA_DICT_FLAG_SORTED);
		} else {
			free((char *) item->value);
			item->value = copy ? strdup(value) : value;
		}
	}
	return 1;
exit_noupdate:
	if (!copy)
		free(value);
	return 0;
}

/** Set a property value
 *
 * \param properties the properties to change
 * \param key a key
 * \param value a value or NULL to remove the key
 * \return 1 if the properties were changed. 0 if nothing was changed because
 *  the property already existed with the same value or because the key to remove
 *  did not exist.
 *
 * Set the property in \a properties with \a key to \a value. Any previous value
 * of \a key will be overwritten. When \a value is NULL, the key will be
 * removed.
 */
SPA_EXPORT
int pw_properties_set(struct pw_properties *properties, const char *key, const char *value)
{
	return do_replace(properties, key, (char*)value, true);
}

SPA_EXPORT
int pw_properties_setva(struct pw_properties *properties,
		   const char *key, const char *format, va_list args)
{
	char *value = NULL;
	if (format != NULL) {
		if (vasprintf(&value, format, args) < 0)
			return -errno;
	}
	return do_replace(properties, key, value, false);
}

/** Set a property value by format
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param format a value
 * \param ... extra arguments
 * \return 1 if the property was changed. 0 if nothing was changed because
 *  the property already existed with the same value or because the key to remove
 *  did not exist.
 *
 * Set the property in \a properties with \a key to the value in printf style \a format
 * Any previous value of \a key will be overwritten.
 */
SPA_EXPORT
int pw_properties_setf(struct pw_properties *properties, const char *key, const char *format, ...)
{
	int res;
	va_list varargs;

	va_start(varargs, format);
	res = pw_properties_setva(properties, key, format, varargs);
	va_end(varargs);

	return res;
}

/** Get a property
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \return the property for \a key or NULL when the key was not found
 *
 * Get the property in \a properties with \a key.
 */
SPA_EXPORT
const char *pw_properties_get(const struct pw_properties *properties, const char *key)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index = find_index(properties, key);

	if (index == -1)
		return NULL;

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->value;
}

/** Fetch a property as uint32_t.
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param value set to the value of the property on success, otherwise left
 * unmodified
 * \return 0 on success or a negative errno otherwise
 * \retval -ENOENT The property does not exist
 * \retval -EINVAL The property is not in the expected format
 */
SPA_EXPORT
int pw_properties_fetch_uint32(const struct pw_properties *properties, const char *key,
			       uint32_t *value)
{
	const char *str = pw_properties_get(properties, key);
	bool success;

	if (!str)
		return -ENOENT;

	success = spa_atou32(str, value, 0);
	if (SPA_UNLIKELY(!success))
		pw_log_warn("Failed to parse \"%s\"=\"%s\" as int32", key, str);

	return success ? 0 : -EINVAL;
}

/** Fetch a property as int32_t
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param value set to the value of the property on success, otherwise left
 * unmodified
 * \return 0 on success or a negative errno otherwise
 * \retval -ENOENT The property does not exist
 * \retval -EINVAL The property is not in the expected format
 */
SPA_EXPORT
int pw_properties_fetch_int32(const struct pw_properties *properties, const char *key,
			      int32_t *value)
{
	const char *str = pw_properties_get(properties, key);
	bool success;

	if (!str)
		return -ENOENT;

	success = spa_atoi32(str, value, 0);
	if (SPA_UNLIKELY(!success))
		pw_log_warn("Failed to parse \"%s\"=\"%s\" as int32", key, str);

	return success ? 0 : -EINVAL;
}

/** Fetch a property as uint64_t.
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param value set to the value of the property on success, otherwise left
 * unmodified
 * \return 0 on success or a negative errno otherwise
 * \retval -ENOENT The property does not exist
 * \retval -EINVAL The property is not in the expected format
 */
SPA_EXPORT
int pw_properties_fetch_uint64(const struct pw_properties *properties, const char *key,
			       uint64_t *value)
{
	const char *str = pw_properties_get(properties, key);
	bool success;

	if (!str)
		return -ENOENT;

	success = spa_atou64(str, value, 0);
	if (SPA_UNLIKELY(!success))
		pw_log_warn("Failed to parse \"%s\"=\"%s\" as uint64", key, str);

	return success ? 0 : -EINVAL;
}

/** Fetch a property as int64_t
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param value set to the value of the property on success, otherwise left
 * unmodified
 * \return 0 on success or a negative errno otherwise
 * \retval -ENOENT The property does not exist
 * \retval -EINVAL The property is not in the expected format
 */
SPA_EXPORT
int pw_properties_fetch_int64(const struct pw_properties *properties, const char *key,
			      int64_t *value)
{
	const char *str = pw_properties_get(properties, key);
	bool success;

	if (!str)
		return -ENOENT;

	success = spa_atoi64(str, value, 0);
	if (SPA_UNLIKELY(!success))
		pw_log_warn("Failed to parse \"%s\"=\"%s\" as int64", key, str);

	return success ? 0 : -EINVAL;
}

/** Fetch a property as boolean value
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param value set to the value of the property on success, otherwise left
 * unmodified
 * \return 0 on success or a negative errno otherwise
 * \retval -ENOENT The property does not exist
 * \retval -EINVAL The property is not in the expected format
 */
SPA_EXPORT
int pw_properties_fetch_bool(const struct pw_properties *properties, const char *key,
			     bool *value)
{
	const char *str = pw_properties_get(properties, key);

	if (!str)
		return -ENOENT;

	*value = spa_atob(str);
	return 0;
}

/** Iterate property values
 *
 * \param properties a \ref pw_properties
 * \param state state
 * \return The next key or NULL when there are no more keys to iterate.
 *
 * Iterate over \a properties, returning each key in turn. \a state should point
 * to a pointer holding NULL to get the first element and will be updated
 * after each iteration. When NULL is returned, all elements have been
 * iterated.
 */
SPA_EXPORT
const char *pw_properties_iterate(const struct pw_properties *properties, void **state)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	uint32_t index;

	if (*state == NULL)
		index = 0;
	else
		index = SPA_PTR_TO_INT(*state);

	if (!pw_array_check_index(&impl->items, index, struct spa_dict_item))
		 return NULL;

	*state = SPA_INT_TO_PTR(index + 1);

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->key;
}

#define NORMAL(c)	((c)->colors ? SPA_ANSI_RESET : "")
#define LITERAL(c)	((c)->colors ? SPA_ANSI_BRIGHT_MAGENTA : "")
#define NUMBER(c)	((c)->colors ? SPA_ANSI_BRIGHT_CYAN : "")
#define STRING(c)	((c)->colors ? SPA_ANSI_BRIGHT_GREEN : "")
#define KEY(c)		((c)->colors ? SPA_ANSI_BRIGHT_BLUE : "")
#define CONTAINER(c)	((c)->colors ? SPA_ANSI_BRIGHT_YELLOW : "")

struct dump_config {
	FILE *file;
	int indent;
	const char *sep;
	bool colors;
	bool recurse;
};

static int encode_string(struct dump_config *c, const char *before,
		const char *val, int size, const char *after)
{
	FILE *f = c->file;
	int i, len = 0;
	len += fprintf(f, "%s\"", before);
	for (i = 0; i < size; i++) {
		char v = val[i];
		switch (v) {
		case '\n':
			len += fprintf(f, "\\n");
			break;
		case '\r':
			len += fprintf(f, "\\r");
			break;
		case '\b':
			len += fprintf(f, "\\b");
			break;
		case '\t':
			len += fprintf(f, "\\t");
			break;
		case '\f':
			len += fprintf(f, "\\f");
			break;
		case '\\': case '"':
			len += fprintf(f, "\\%c", v);
			break;
		default:
			if (v > 0 && v < 0x20)
				len += fprintf(f, "\\u%04x", v);
			else
				len += fprintf(f, "%c", v);
			break;
		}
	}
	len += fprintf(f, "\"%s", after);
	return len-1;
}

static int dump(struct dump_config *c, int indent, struct spa_json *it, const char *value, int len)
{
	FILE *file = c->file;
	struct spa_json sub;
	int count = 0;
	char key[1024];

	if (value == NULL || len == 0) {
		fprintf(file, "%snull%s", LITERAL(c), NORMAL(c));
	} else if (spa_json_is_container(value, len) && !c->recurse) {
		spa_json_enter_container(it, &sub, value[0]);
		if (spa_json_container_len(&sub, value, len) == len)
			fprintf(file, "%s%.*s%s", CONTAINER(c), len, value, NORMAL(c));
		else
			encode_string(c, STRING(c), value, len, NORMAL(c));
	} else if (spa_json_is_array(value, len)) {
		fprintf(file, "[");
		spa_json_enter(it, &sub);
		indent += c->indent;
		while ((len = spa_json_next(&sub, &value)) > 0) {
			fprintf(file, "%s%s%*s", count++ > 0 ? "," : "",
					c->sep, indent, "");
			dump(c, indent, &sub, value, len);
		}
		indent -= c->indent;
		fprintf(file, "%s%*s]", count > 0 ? c->sep : "",
				count > 0 ? indent : 0, "");
	} else if (spa_json_is_object(value, len)) {
		fprintf(file, "{");
		spa_json_enter(it, &sub);
		indent += c->indent;
		while (spa_json_get_string(&sub, key, sizeof(key)) > 0) {
			fprintf(file, "%s%s%*s",
					count++ > 0 ? "," : "",
					c->sep, indent, "");
			encode_string(c, KEY(c), key, strlen(key), NORMAL(c));
			fprintf(file, ": ");
			if ((len = spa_json_next(&sub, &value)) <= 0)
				break;
			dump(c, indent, &sub, value, len);
		}
		indent -= c->indent;
		fprintf(file, "%s%*s}", count > 0 ? c->sep : "",
				count > 0 ? indent : 0, "");
	} else if (spa_json_is_null(value, len) ||
	    spa_json_is_bool(value, len)) {
		fprintf(file, "%s%.*s%s", LITERAL(c), len, value, NORMAL(c));
	} else if (spa_json_is_int(value, len) ||
	    spa_json_is_float(value, len)) {
		fprintf(file, "%s%.*s%s", NUMBER(c), len, value, NORMAL(c));
	} else if (spa_json_is_string(value, len)) {
		fprintf(file, "%s%.*s%s", STRING(c), len, value, NORMAL(c));
	} else {
		encode_string(c, STRING(c), value, len, NORMAL(c));
	}
	return 0;
}

SPA_EXPORT
int pw_properties_serialize_dict(FILE *f, const struct spa_dict *dict, uint32_t flags)
{
	const struct spa_dict_item *it;
	int count = 0;
	struct dump_config cfg = {
		.file = f,
		.indent = flags & PW_PROPERTIES_FLAG_NL ? 2 : 0,
		.sep = flags & PW_PROPERTIES_FLAG_NL ? "\n" : " ",
		.colors = SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_COLORS),
		.recurse = SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_RECURSE),
	}, *c = &cfg;
	const char *enc = flags & PW_PROPERTIES_FLAG_ARRAY ? "[]" : "{}";

	if (SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_ENCLOSE))
		fprintf(f, "%c", enc[0]);

	spa_dict_for_each(it, dict) {
		char key[1024];
		int len;
		const char *value;
		struct spa_json sub;

		fprintf(f, "%s%s%*s", count == 0 ? "" : ",", c->sep, c->indent, "");

		if (!(flags & PW_PROPERTIES_FLAG_ARRAY)) {
			if (spa_json_encode_string(key, sizeof(key)-1, it->key) >= (int)sizeof(key)-1)
				continue;
			fprintf(f, "%s%s%s: ", KEY(c), key, NORMAL(c));
		}
		value = it->value;
		len = value ? strlen(value) : 0;
		spa_json_init(&sub, value, len);
		if (c->recurse && spa_json_next(&sub, &value) < 0)
			break;

		dump(c, c->indent, &sub, value, len);
		count++;
	}
	if (SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_ENCLOSE))
		fprintf(f, "%s%c", c->sep, enc[1]);
	return count;
}
