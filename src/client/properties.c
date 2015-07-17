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

#include "client/properties.h"

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

  copy = pinos_properties_new (NULL);
  g_hash_table_foreach (properties->hashtable, (GHFunc) copy_func, copy->hashtable);

  return copy;
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

const gchar *
pinos_properties_get (PinosProperties *properties,
                      const gchar     *key)
{
  g_return_val_if_fail (properties != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  return g_hash_table_lookup (properties->hashtable, key);
}
void
pinos_properties_remove (PinosProperties *properties,
                         const gchar     *key)
{
  g_return_if_fail (properties != NULL);
  g_return_if_fail (key != NULL);

  g_hash_table_remove (properties->hashtable, key);
}

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

GVariant *
pinos_properties_to_variant (PinosProperties *properties)
{
  GVariantBuilder builder;

  g_return_val_if_fail (properties != NULL, NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_hash_table_foreach (properties->hashtable, (GHFunc) add_to_variant, &builder);
  return g_variant_builder_end (&builder);
}

PinosProperties *
pinos_properties_from_variant (GVariant *variant)
{
  PinosProperties *props;
  GVariantIter iter;
  GVariant *value;
  gchar *key;

  g_return_val_if_fail (variant != NULL, NULL);

  props = pinos_properties_new (NULL);

  g_variant_iter_init (&iter, variant);
  while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
    g_hash_table_replace (props->hashtable,
                          g_strdup (key),
                          g_variant_dup_string (value, NULL));

  return props;
}

G_DEFINE_BOXED_TYPE (PinosProperties, pinos_properties,
        pinos_properties_copy, pinos_properties_free);

