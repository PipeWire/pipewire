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

#include <glib-object.h>

#include "pinos/client/pinos.h"
#include "pinos/client/properties.h"

struct _PinosProperties {
  GHashTable *hashtable;
};

static void
copy_func (const char *key, const char *value, GHashTable *copy)
{
  g_hash_table_insert (copy, g_strdup (key), g_strdup (value));
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

  props = g_new (PinosProperties, 1);
  props->hashtable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  va_start (varargs, key);
  while (key != NULL) {
    value = va_arg (varargs, char *);
    copy_func (key, value, props->hashtable);
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

  g_return_val_if_fail (properties != NULL, NULL);

  copy = pinos_properties_new (NULL, NULL);
  g_hash_table_foreach (properties->hashtable, (GHFunc) copy_func, copy->hashtable);

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
  g_return_if_fail (properties != NULL);

  g_hash_table_unref (properties->hashtable);
  g_free (properties);
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
  g_return_if_fail (properties != NULL);
  g_return_if_fail (key != NULL);

  if (value == NULL)
    g_hash_table_remove (properties->hashtable, key);
  else
    g_hash_table_replace (properties->hashtable, g_strdup (key), g_strdup (value));
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

  g_return_if_fail (properties != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (format != NULL);

  va_start (varargs, format);
  g_hash_table_replace (properties->hashtable,
                        g_strdup (key),
                        g_strdup_vprintf (format, varargs));
  va_end (varargs);
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
  g_return_val_if_fail (properties != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  return g_hash_table_lookup (properties->hashtable, key);
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
  static void * dummy = GINT_TO_POINTER (1);
  const char *res = NULL;
  GList *items;

  g_return_val_if_fail (properties != NULL, NULL);
  g_return_val_if_fail (state != NULL, NULL);

  if (*state == dummy)
    return NULL;

  if (*state == NULL) {
    items = g_hash_table_get_keys (properties->hashtable);
  } else {
    items = *state;
  }

  if (items) {
    res = items->data;
    items = g_list_delete_link (items, items);
    if (items == NULL)
      items = dummy;
  }
  *state = items;

  return res;
}
