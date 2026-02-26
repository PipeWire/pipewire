/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdarg.h>

#include <spa/utils/ansi.h>
#include <spa/utils/json-builder.h>
#include <spa/utils/string.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>
#include <spa/debug/log.h>

#define PW_API_PROPERTIES SPA_EXPORT
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

static int add_item(struct properties *impl, const char *key, bool take_key, const char *value, bool take_value)
{
	struct spa_dict_item *item;
	const char *k, *v;

	k = take_key ? key : NULL;
	v = take_value ? value: NULL;

	if (!take_key && key && (k = strdup(key)) == NULL)
		goto error;
	if (!take_value && value && (v = strdup(value)) == NULL)
		goto error;

	item = pw_array_add(&impl->items, sizeof(struct spa_dict_item));
	if (item == NULL)
		goto error;

	item->key = k;
	item->value = v;
	return 0;

error:
	free((char*)k);
	free((char*)v);
	return -errno;
}

static void update_dict(struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	properties->dict.items = impl->items.data;
	properties->dict.n_items = pw_array_get_len(&impl->items, struct spa_dict_item);
}

static void clear_item(struct spa_dict_item *item)
{
	free((char *) item->key);
	free((char *) item->value);
}

static void properties_init(struct properties *impl, int prealloc)
{
	pw_array_init(&impl->items, 16);
	pw_array_ensure_size(&impl->items, sizeof(struct spa_dict_item) * prealloc);
}

static struct properties *properties_new(int prealloc)
{
	struct properties *impl;

	if ((impl = calloc(1, sizeof(struct properties))) == NULL)
		return NULL;

	properties_init(impl, prealloc);
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
	int res = 0;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	va_start(varargs, key);
	while (key != NULL) {
		value = va_arg(varargs, char *);
		if (value && key[0])
			if ((res = add_item(impl, key, false, value, false)) < 0)
				goto error;
		key = va_arg(varargs, char *);
	}
	va_end(varargs);
	update_dict(&impl->this);

	return &impl->this;
error:
	pw_properties_free(&impl->this);
	errno = -res;
	return NULL;
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
	int res;

	impl = properties_new(SPA_ROUND_UP_N(dict->n_items, 16));
	if (impl == NULL)
		return NULL;

	for (i = 0; i < dict->n_items; i++) {
		const struct spa_dict_item *it = &dict->items[i];
		if (it->key != NULL && it->key[0] && it->value != NULL)
			if ((res = add_item(impl, it->key, false, it->value, false)) < 0)
				goto error;
	}
	update_dict(&impl->this);

	return &impl->this;

error:
	pw_properties_free(&impl->this);
	errno = -res;
	return NULL;
}

static int do_replace(struct pw_properties *properties, const char *key, bool take_key,
		const char *value, bool take_value)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	struct spa_dict_item *item;
	int res = 0;

	if (key == NULL || key[0] == 0)
		goto exit_noupdate;

	item = (struct spa_dict_item*) spa_dict_lookup_item(&properties->dict, key);

	if (item == NULL) {
		if (value == NULL)
			goto exit_noupdate;
		if ((res = add_item(impl, key, take_key, value, take_value)) < 0)
			return res;
		SPA_FLAG_CLEAR(properties->dict.flags, SPA_DICT_FLAG_SORTED);
	} else {
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
			SPA_FLAG_CLEAR(properties->dict.flags, SPA_DICT_FLAG_SORTED);
		} else {
			char *v = NULL;
			if (!take_value && value && (v = strdup(value)) == NULL) {
				res = -errno;
				goto exit_noupdate;
			}
			free((char *) item->value);
			item->value = take_value ? value : v;
		}
		if (take_key)
			free((char*)key);
	}
	update_dict(properties);
	return 1;

exit_noupdate:
	if (take_key)
		free((char*)key);
	if (take_value)
		free((char*)value);
	return res;
}

static int update_string(struct pw_properties *props, const char *str, size_t size,
		int *count, struct spa_error_location *loc)
{
	struct spa_json it[1];
	char key[1024];
	struct spa_error_location el;
	bool err;
	int len, res, cnt = 0;
	struct properties changes;
	const char *value;

	if ((res = spa_json_begin_object_relax(&it[0], str, size)) <= 0)
		return res;

	if (props)
		properties_init(&changes, 16);

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &value)) > 0) {
		char *val = NULL;

		if (spa_json_is_null(value, len))
			val = NULL;
		else {
			if (spa_json_is_container(value, len))
				len = spa_json_container_len(&it[0], value, len);
			if (len <= 0)
				break;

			if (props) {
				if ((val = malloc(len+1)) == NULL) {
					it[0].state = SPA_JSON_ERROR_FLAG;
					break;
				}
				spa_json_parse_stringn(value, len, val, len+1);
			}
		}
		if (props) {
			const struct spa_dict_item *item;
			item = spa_dict_lookup_item(&props->dict, key);
			if (item && spa_streq(item->value, val)) {
				free(val);
				continue;
			}
			/* item changed or added, apply changes later */
			if ((res = add_item(&changes, key, false, val, true)) < 0) {
				errno = -res;
				it[0].state = SPA_JSON_ERROR_FLAG;
				break;
			}
		}
	}
	if ((err = spa_json_get_error(&it[0], str, &el))) {
		if (loc == NULL)
			spa_debug_log_error_location(pw_log_get(), SPA_LOG_LEVEL_WARN,
					&el, "error parsing more than %d properties: %s",
					cnt, el.reason);
		else
			*loc = el;
	}
	if (props) {
		struct spa_dict_item *item;
		if (loc == NULL || !err) {
			pw_array_for_each(item, &changes.items)
				if ((res = do_replace(props, item->key, true, item->value, true)) < 0)
					pw_log_warn("error updating property: %s", spa_strerror(res));
				else
					cnt += res;

		} else {
			pw_array_for_each(item, &changes.items)
				clear_item(item);
		}
		pw_array_clear(&changes.items);
	}
	if (count)
		*count = cnt;

	return !err;
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
	int count = 0;
	update_string(props, str, size, &count, NULL);
	return count;
}

/** Check \a str is a well-formed properties JSON string and update
 * the properties on success.
 *
 * \a str should be a whitespace separated list of key=value
 * strings or a json object, see pw_properties_new_string().
 *
 * When the check fails, this function will not update \a props.
 *
 * \param props  The properties to attempt to update, maybe be NULL
 *            to simply check the JSON string.
 * \param str  The JSON object with new values
 * \param size  The length of the JSON string.
 * \param loc  Return value for parse error location
 * \return a negative value when string is not valid and \a loc contains
 *         the error location or the number of updated properties in
 *         \a props.
 * \since 1.1.0
 */
SPA_EXPORT
int pw_properties_update_string_checked(struct pw_properties *props,
		const char *str, size_t size, struct spa_error_location *loc)
{
	int count = 0;
	if (!update_string(props, str, size, &count, loc))
		return -EINVAL;
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

SPA_EXPORT
struct pw_properties *
pw_properties_new_string_checked(const char *object, size_t size, struct spa_error_location *loc)
{
	struct properties *impl;
	int res;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	if ((res = pw_properties_update_string_checked(&impl->this, object, size, loc)) < 0)
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
	int i, res, changed = 0;
	const char *str;

	for (i = 0; keys[i]; i++) {
		if ((str = spa_dict_lookup(dict, keys[i])) != NULL) {
			if ((res = pw_properties_set(props, keys[i], str)) < 0)
				pw_log_warn("error updating property %s:%s: %s",
						keys[i], str, spa_strerror(res));
			else
				changed += res;
		}
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
	int res, changed = 0;

	spa_dict_for_each(it, dict) {
		if (ignore == NULL || !has_key(ignore, it->key)) {
			if ((res = pw_properties_set(props, it->key, it->value)) < 0)
				pw_log_warn("error updating property %s:%s: %s",
						it->key, it->value, spa_strerror(res));
			else
				changed += res;
		}
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
	int res, changed = 0;

	spa_dict_for_each(it, dict) {
		if ((res = pw_properties_set(props, it->key, it->value)) < 0)
			pw_log_warn("error updating property %s:%s: %s",
					it->key, it->value, spa_strerror(res));
		else
			changed += res;
	}
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
	int res, added = 0;

	for (i = 0; i < dict->n_items; i++) {
		const struct spa_dict_item *it = &dict->items[i];
		if (pw_properties_get(props, it->key) == NULL) {
			if ((res = pw_properties_set(props, it->key, it->value)) < 0)
				pw_log_warn("error updating property %s:%s: %s",
						it->key, it->value, spa_strerror(res));
			else
				added += res;
		}
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
	int res, added = 0;
	const char *str;

	for (i = 0; keys[i]; i++) {
		if ((str = spa_dict_lookup(dict, keys[i])) == NULL)
			continue;
		if (pw_properties_get(props, keys[i]) == NULL) {
			if ((res = pw_properties_set(props, keys[i], str)) < 0)
				pw_log_warn("error updating property %s:%s: %s",
						keys[i], str, spa_strerror(res));
			else
				added += res;
		}
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

/** Set a property value
 *
 * \param properties the properties to change
 * \param key a key
 * \param value a value or NULL to remove the key
 * \return 1 if the properties were changed. 0 if nothing was changed because
 *  the property already existed with the same value or because the key to remove
 *  did not exist. < 0 if an error occurred and nothing was changed.
 *
 * Set the property in \a properties with \a key to \a value. Any previous value
 * of \a key will be overwritten. When \a value is NULL, the key will be
 * removed.
 */
SPA_EXPORT
int pw_properties_set(struct pw_properties *properties, const char *key, const char *value)
{
	return do_replace(properties, key, false, (char*)value, false);
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
	return do_replace(properties, key, false, value, true);
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
	return spa_dict_lookup(&properties->dict, key);
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

SPA_EXPORT
int pw_properties_serialize_dict(FILE *f, const struct spa_dict *dict, uint32_t flags)
{
	const struct spa_dict_item *it;
	int count = 0, fl = 0;
	struct spa_json_builder b;
	bool array = flags & PW_PROPERTIES_FLAG_ARRAY;
	bool recurse = flags & PW_PROPERTIES_FLAG_RECURSE;

	if (flags & PW_PROPERTIES_FLAG_NL)
		fl |= SPA_JSON_BUILDER_FLAG_PRETTY;
	if (flags & PW_PROPERTIES_FLAG_COLORS)
		fl |= SPA_JSON_BUILDER_FLAG_COLOR;
	if (flags & PW_PROPERTIES_FLAG_SIMPLE)
		fl |= SPA_JSON_BUILDER_FLAG_SIMPLE;

	spa_json_builder_file(&b, f, fl);

	if (SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_ENCLOSE))
		spa_json_builder_array_push(&b, array ? "[" : "{");

	spa_dict_for_each(it, dict) {
		spa_json_builder_object_value(&b, recurse, array ? NULL : it->key, it->value);
		count++;
	}
	if (SPA_FLAG_IS_SET(flags, PW_PROPERTIES_FLAG_ENCLOSE))
		spa_json_builder_pop(&b, array ? "]" : "}");
	return count;
}
