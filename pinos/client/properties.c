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

#include "pinos/client/properties.h"

struct _PinosProperties {
  GHashTable *hashtable;
};

static void
copy_func (const gchar *key, const gchar *value, GHashTable *copy)
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
pinos_properties_new (const gchar *key, ...)
{
  PinosProperties *props;
  va_list varargs;
  const gchar *value;

  props = g_new (PinosProperties, 1);
  props->hashtable = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  va_start (varargs, key);
  while (key != NULL) {
    value = va_arg (varargs, gchar *);
    copy_func (key, value, props->hashtable);
    key = va_arg (varargs, gchar *);
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
    const gchar *key;
    gpointer state = NULL;

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
 * of @key will be overwritten.
 */
void
pinos_properties_set (PinosProperties *properties,
                      const gchar     *key,
                      const gchar     *value)
{
  g_return_if_fail (properties != NULL);
  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

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
                       const gchar     *key,
                       const gchar     *format,
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
const gchar *
pinos_properties_get (PinosProperties *properties,
                      const gchar     *key)
{
  g_return_val_if_fail (properties != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  return g_hash_table_lookup (properties->hashtable, key);
}

/**
 * pinos_properties_remove:
 * @properties: a #PinosProperties
 * @key: a key
 *
 * Remove the property in @properties with @key.
 */
void
pinos_properties_remove (PinosProperties *properties,
                         const gchar     *key)
{
  g_return_if_fail (properties != NULL);
  g_return_if_fail (key != NULL);

  g_hash_table_remove (properties->hashtable, key);
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
const gchar *
pinos_properties_iterate (PinosProperties     *properties,
                          gpointer            *state)
{
  static gpointer dummy = GINT_TO_POINTER (1);
  const gchar *res = NULL;
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

static void
add_to_variant (const gchar *key, const gchar *value, GVariantBuilder *b)
{
  g_variant_builder_add (b, "{sv}", key, g_variant_new_string (value));
}

/**
 * pinos_properties_init_builder:
 * @properties: a #PinosProperties
 * @builder: a #GVariantBuilder
 *
 * Initialize the @builder of type a{sv} and add @properties to it.
 *
 * Returns: %TRUE if @builder could be initialized.
 */
gboolean
pinos_properties_init_builder (PinosProperties *properties,
                               GVariantBuilder *builder)
{
  g_return_val_if_fail (properties != NULL, FALSE);
  g_return_val_if_fail (builder != NULL, FALSE);

  g_variant_builder_init (builder, G_VARIANT_TYPE ("a{sv}"));
  g_hash_table_foreach (properties->hashtable, (GHFunc) add_to_variant, builder);

  return TRUE;
}

/**
 * pinos_properties_to_variant:
 * @properties: a #PinosProperties
 *
 * Convert @properties to a #GVariant of type a{sv}
 *
 * Returns: a new #GVariant of @properties. use g_variant_unref() after
 *          use.
 */
GVariant *
pinos_properties_to_variant (PinosProperties *properties)
{
  GVariantBuilder builder;

  if (!pinos_properties_init_builder (properties, &builder))
    return NULL;

  return g_variant_builder_end (&builder);
}

/**
 * pinos_properties_from_variant:
 * @variant: a #GVariant
 *
 * Convert @variant to a #PinosProperties
 *
 * Returns: a new #PinosProperties of @variant. use pinos_properties_free()
 *          after use.
 */
PinosProperties *
pinos_properties_from_variant (GVariant *variant)
{
  PinosProperties *props;
  GVariantIter iter;
  GVariant *value;
  gchar *key;

  g_return_val_if_fail (variant != NULL, NULL);

  props = pinos_properties_new (NULL, NULL);

  g_variant_iter_init (&iter, variant);
  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    g_hash_table_replace (props->hashtable,
                          g_strdup (key),
                          g_variant_dup_string (value, NULL));

  return props;
}

G_DEFINE_BOXED_TYPE (PinosProperties, pinos_properties,
        pinos_properties_copy, pinos_properties_free);
