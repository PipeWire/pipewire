/* Pinos
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

#include "pinos/client/pinos.h"
#include "pinos/client/properties.h"

typedef struct {
  char *key;
  char *value;
} PropItem;

struct _PinosProperties {
  PinosArray items;
};

static void
add_func (PinosProperties *props, char *key, char *value)
{
  PropItem *item;
  item = pinos_array_add (&props->items, sizeof (PropItem));
  item->key = key;
  item->value = value;
}

static void
clear_item (PropItem *item)
{
  free (item->key);
  free (item->value);
}

static int
find_index (PinosProperties *props, const char *key)
{
  int i, len = pinos_array_get_len (&props->items, PropItem);

  for (i = 0; i < len; i++) {
    PropItem *item = pinos_array_get_unchecked (&props->items, i, PropItem);
    if (strcmp (item->key, key) == 0)
      return i;
  }
  return -1;
}

/**
 * pinos_properties_new:
 * @key: first key
 * @...: value
 *
 * Make a new #PinosProperties with given, NULL-terminated key/value pairs.
 *
 * Returns: a new #PinosProperties
 */
PinosProperties *
pinos_properties_new (const char *key, ...)
{
  PinosProperties *props;
  va_list varargs;
  const char *value;

  props = calloc (1, sizeof (PinosProperties));
  pinos_array_init (&props->items);

  va_start (varargs, key);
  while (key != NULL) {
    value = va_arg (varargs, char *);
    add_func (props, strdup (key), strdup (value));
    key = va_arg (varargs, char *);
  }
  va_end (varargs);

  return props;
}

/**
 * pinos_properties_copy:
 * @properties: a #PinosProperties
 *
 * Make a copy of @properties.
 *
 * Returns: a copy of @properties
 */
PinosProperties *
pinos_properties_copy (PinosProperties *properties)
{
  PinosProperties *copy;
  PropItem *item;

  copy = pinos_properties_new (NULL, NULL);
  pinos_array_for_each (item, &properties->items)
    add_func (copy, strdup (item->key), strdup (item->value));

  return copy;
}

PinosProperties *
pinos_properties_merge (PinosProperties *oldprops,
                        PinosProperties *newprops)
{
  PinosProperties *res = NULL;

  if (oldprops == NULL) {
    if (newprops == NULL)
      res = NULL;
    else
      res = pinos_properties_copy (newprops);
  } else if (newprops == NULL) {
    res = pinos_properties_copy (oldprops);
  } else {
    const char *key;
    void * state = NULL;

    res = pinos_properties_copy (oldprops);
    while ((key = pinos_properties_iterate (newprops, &state))) {
       pinos_properties_set (res,
                             key,
                             pinos_properties_get (newprops, key));
    }
  }
  return res;
}

/**
 * pinos_properties_free:
 * @properties: a #PinosProperties
 *
 * Free @properties
 */
void
pinos_properties_free (PinosProperties *properties)
{
  PropItem *item;

  pinos_array_for_each (item, &properties->items)
    clear_item (item);

  pinos_array_clear (&properties->items);
  free (properties);
}

static void
do_replace (PinosProperties *properties,
            char            *key,
            char            *value)
{
  int index = find_index (properties, key);

  if (index == -1) {
    add_func (properties, key, value);
  } else {
    PropItem *item = pinos_array_get_unchecked (&properties->items, index, PropItem);

    clear_item (item);
    if (value == NULL) {
      PropItem *other = pinos_array_get_unchecked (&properties->items,
                                                   pinos_array_get_len (&properties->items, PropItem) - 1,
                                                   PropItem);
      item->key = other->key;
      item->value = other->value;
      properties->items.size -= sizeof (PropItem);
    } else {
      item->key = key;
      item->value = value;
    }
  }
}

/**
 * pinos_properties_set:
 * @properties: a #PinosProperties
 * @key: a key
 * @value: a value
 *
 * Set the property in @properties with @key to @value. Any previous value
 * of @key will be overwritten. When @value is %NULL, the key will be
 * removed.
 */
void
pinos_properties_set (PinosProperties *properties,
                      const char      *key,
                      const char      *value)
{
  do_replace (properties, strdup (key), value ? strdup (value) : NULL);
}

/**
 * pinos_properties_setf:
 * @properties: a #PinosProperties
 * @key: a key
 * @format: a value
 * @...: extra arguments
 *
 * Set the property in @properties with @key to the value in printf style @format
 * Any previous value of @key will be overwritten.
 */
void
pinos_properties_setf (PinosProperties *properties,
                       const char      *key,
                       const char      *format,
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
 * pinos_properties_get:
 * @properties: a #PinosProperties
 * @key: a key
 *
 * Get the property in @properties with @key.
 *
 * Returns: the property for @key or %NULL when the key was not found
 */
const char *
pinos_properties_get (PinosProperties *properties,
                      const char      *key)
{
  int index = find_index (properties, key);

  if (index == -1)
    return NULL;

  return pinos_array_get_unchecked (&properties->items, index, PropItem)->value;
}

/**
 * pinos_properties_iterate:
 * @properties: a #PinosProperties
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
pinos_properties_iterate (PinosProperties     *properties,
                          void               **state)
{
  unsigned int index;

  if (*state == NULL)
    index = 0;
  else
    index = SPA_PTR_TO_INT (*state);

  if (!pinos_array_check_index (&properties->items, index, PropItem))
    return NULL;

  *state = SPA_INT_TO_PTR (index + 1);

  return pinos_array_get_unchecked (&properties->items, index, PropItem)->key;
}
