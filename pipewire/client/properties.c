/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include "pipewire/client/pipewire.h"
#include "pipewire/client/properties.h"

struct properties {
  struct pw_properties this;

  struct pw_array items;
};

static void
add_func (struct pw_properties *this, char *key, char *value)
{
  struct spa_dict_item *item;
  struct properties *impl = SPA_CONTAINER_OF (this, struct properties, this);

  item = pw_array_add (&impl->items, sizeof (struct spa_dict_item));
  item->key = key;
  item->value = value;

  this->dict.items = impl->items.data;
  this->dict.n_items = pw_array_get_len (&impl->items, struct spa_dict_item);
}

static void
clear_item (struct spa_dict_item *item)
{
  free ((char*)item->key);
  free ((char*)item->value);
}

static int
find_index (struct pw_properties *this, const char *key)
{
  struct properties *impl = SPA_CONTAINER_OF (this, struct properties, this);
  int i, len = pw_array_get_len (&impl->items, struct spa_dict_item);

  for (i = 0; i < len; i++) {
    struct spa_dict_item *item = pw_array_get_unchecked (&impl->items, i, struct spa_dict_item);
    if (strcmp (item->key, key) == 0)
      return i;
  }
  return -1;
}

/**
 * pw_properties_new:
 * @key: first key
 * @...: value
 *
 * Make a new #struct pw_properties with given, NULL-terminated key/value pairs.
 *
 * Returns: a new #struct pw_properties
 */
struct pw_properties *
pw_properties_new (const char *key, ...)
{
  struct properties *impl;
  va_list varargs;
  const char *value;

  impl = calloc (1, sizeof (struct properties));
  if (impl == NULL)
    return NULL;

  pw_array_init (&impl->items, 16);

  va_start (varargs, key);
  while (key != NULL) {
    value = va_arg (varargs, char *);
    add_func (&impl->this, strdup (key), strdup (value));
    key = va_arg (varargs, char *);
  }
  va_end (varargs);

  return &impl->this;
}

/**
 * pw_properties_new_dict:
 * @dict: a dict
 *
 * Make a new #struct pw_properties with given @dict.
 *
 * Returns: a new #struct pw_properties
 */
struct pw_properties *
pw_properties_new_dict (const struct spa_dict *dict)
{
  uint32_t i;
  struct properties *impl;

  impl = calloc (1, sizeof (struct properties));
  if (impl == NULL)
    return NULL;

  pw_array_init (&impl->items, 16);

  for (i = 0; i < dict->n_items; i++)
    add_func (&impl->this, strdup (dict->items[i].key), strdup (dict->items[i].value));

  return &impl->this;
}

/**
 * pw_properties_copy:
 * @properties: a #struct pw_properties
 *
 * Make a copy of @properties.
 *
 * Returns: a copy of @properties
 */
struct pw_properties *
pw_properties_copy (struct pw_properties *properties)
{
  struct properties *impl = SPA_CONTAINER_OF (properties, struct properties, this);
  struct pw_properties *copy;
  struct spa_dict_item *item;

  copy = pw_properties_new (NULL, NULL);
  if (copy == NULL)
    return NULL;

  pw_array_for_each (item, &impl->items)
    add_func (copy, strdup (item->key), strdup (item->value));

  return copy;
}

struct pw_properties *
pw_properties_merge (struct pw_properties *oldprops,
                     struct pw_properties *newprops)
{
  struct pw_properties *res = NULL;

  if (oldprops == NULL) {
    if (newprops == NULL)
      res = NULL;
    else
      res = pw_properties_copy (newprops);
  } else if (newprops == NULL) {
    res = pw_properties_copy (oldprops);
  } else {
    const char *key;
    void * state = NULL;

    res = pw_properties_copy (oldprops);
    if (res == NULL)
      return NULL;

    while ((key = pw_properties_iterate (newprops, &state))) {
       pw_properties_set (res,
                             key,
                             pw_properties_get (newprops, key));
    }
  }
  return res;
}

/**
 * pw_properties_free:
 * @properties: a #struct pw_properties
 *
 * Free @properties
 */
void
pw_properties_free (struct pw_properties *properties)
{
  struct properties *impl = SPA_CONTAINER_OF (properties, struct properties, this);
  struct spa_dict_item *item;

  pw_array_for_each (item, &impl->items)
    clear_item (item);

  pw_array_clear (&impl->items);
  free (impl);
}

static void
do_replace (struct pw_properties *properties,
            char                 *key,
            char                 *value)
{
  struct properties *impl = SPA_CONTAINER_OF (properties, struct properties, this);
  int index = find_index (properties, key);

  if (index == -1) {
    add_func (properties, key, value);
  } else {
    struct spa_dict_item *item = pw_array_get_unchecked (&impl->items, index, struct spa_dict_item);

    clear_item (item);
    if (value == NULL) {
      struct spa_dict_item *other = pw_array_get_unchecked (&impl->items,
                                                   pw_array_get_len (&impl->items, struct spa_dict_item) - 1,
                                                   struct spa_dict_item);
      item->key = other->key;
      item->value = other->value;
      impl->items.size -= sizeof (struct spa_dict_item);
    } else {
      item->key = key;
      item->value = value;
    }
  }
}

/**
 * pw_properties_set:
 * @properties: a #struct pw_properties
 * @key: a key
 * @value: a value
 *
 * Set the property in @properties with @key to @value. Any previous value
 * of @key will be overwritten. When @value is %NULL, the key will be
 * removed.
 */
void
pw_properties_set (struct pw_properties *properties,
                   const char           *key,
                   const char           *value)
{
  do_replace (properties, strdup (key), value ? strdup (value) : NULL);
}

/**
 * pw_properties_setf:
 * @properties: a #struct pw_properties
 * @key: a key
 * @format: a value
 * @...: extra arguments
 *
 * Set the property in @properties with @key to the value in printf style @format
 * Any previous value of @key will be overwritten.
 */
void
pw_properties_setf (struct pw_properties *properties,
                    const char           *key,
                    const char           *format,
                    ...)
{
  va_list varargs;
  char *value;

  va_start (varargs, format);
  vasprintf (&value, format, varargs);
  va_end (varargs);

  do_replace (properties, strdup (key), value);
}

/**
 * pw_properties_get:
 * @properties: a #struct pw_properties
 * @key: a key
 *
 * Get the property in @properties with @key.
 *
 * Returns: the property for @key or %NULL when the key was not found
 */
const char *
pw_properties_get (struct pw_properties *properties,
                   const char           *key)
{
  struct properties *impl = SPA_CONTAINER_OF (properties, struct properties, this);
  int index = find_index (properties, key);

  if (index == -1)
    return NULL;

  return pw_array_get_unchecked (&impl->items, index, struct spa_dict_item)->value;
}

/**
 * pw_properties_iterate:
 * @properties: a #struct pw_properties
 * @state: state
 *
 * Iterate over @properties, returning each key in turn. @state should point
 * to a pointer holding %NULL to get the first element and will be updated
 * after each iteration. When %NULL is returned, all elements have been
 * iterated.
 *
 * Aborting the iteration before %NULL is returned might cause memory leaks.
 *
 * Returns: The next key or %NULL when there are no more keys to iterate.
 */
const char *
pw_properties_iterate (struct pw_properties  *properties,
                       void                 **state)
{
  struct properties *impl = SPA_CONTAINER_OF (properties, struct properties, this);
  uint32_t index;

  if (*state == NULL)
    index = 0;
  else
    index = SPA_PTR_TO_INT (*state);

  if (!pw_array_check_index (&impl->items, index, struct spa_dict_item))
    return NULL;

  *state = SPA_INT_TO_PTR (index + 1);

  return pw_array_get_unchecked (&impl->items, index, struct spa_dict_item)->key;
}
