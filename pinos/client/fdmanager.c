/* GStreamer
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fdmanager.h"

struct _PinosFdManagerPrivate
{
  GMutex lock;
  GHashTable *object_ids;
  GHashTable *client_ids;
  volatile gint id_counter;
};

#define PINOS_FD_MANAGER_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_FD_MANAGER, PinosFdManagerPrivate))

G_DEFINE_TYPE (PinosFdManager, pinos_fd_manager, G_TYPE_OBJECT);

static GMutex manager_lock;
static GHashTable *managers;

typedef struct {
  guint32 id;
  gint refcount;
  gpointer obj;
  GDestroyNotify notify;
} ObjectId;

static ObjectId *
object_id_new (guint32 id, gpointer obj, GDestroyNotify notify)
{
  ObjectId *oid;

  oid = g_slice_new (ObjectId);
  oid->id = id;
  oid->refcount = 1;
  oid->obj = obj;
  oid->notify = notify;

  return oid;
}

static void
object_id_free (ObjectId *oid)
{
  g_assert (oid->refcount == 0);
  oid->notify (oid->obj);
  g_slice_free (ObjectId, oid);
}

typedef struct {
  GList *ids;
} ClientIds;

static ClientIds *
client_ids_new (ObjectId *oid)
{
  ClientIds *ids;

  ids = g_slice_new (ClientIds);
  ids->ids = g_list_prepend (NULL, oid);

  return ids;
}

static void
client_ids_free (ClientIds *ids)
{
  g_list_free (ids->ids);
  g_slice_free (ClientIds, ids);
}

/**
 * pinos_fd_manager_get:
 * @type: the manager type
 *
 * Get a manager of @type. There will be a single instance of a #PinosFdManager
 * per @type.
 *
 * Returns: a new reference to the #PinosFdManager for @type.
 */
PinosFdManager *
pinos_fd_manager_get (const gchar *type)
{
  PinosFdManager *manager;

  g_return_val_if_fail (type != NULL, NULL);

  g_type_class_ref (PINOS_TYPE_FD_MANAGER);

  g_mutex_lock (&manager_lock);
  manager = g_hash_table_lookup (managers, type);
  if (manager == NULL) {
    manager = g_object_new (PINOS_TYPE_FD_MANAGER, NULL);
    g_hash_table_insert (managers, g_strdup (type), manager);
  }
  g_mutex_unlock (&manager_lock);

  return g_object_ref (manager);
}

/**
 * pinos_fd_manager_get_id:
 * @manager: a #PinosFdManager
 *
 * Get the next available id from @manager
 *
 * Returns: the next unused id.
 */
guint32
pinos_fd_manager_get_id (PinosFdManager *manager)
{
  PinosFdManagerPrivate *priv;

  g_return_val_if_fail (PINOS_IS_FD_MANAGER (manager), -1);

  priv = manager->priv;

  return (guint32) g_atomic_int_add (&priv->id_counter, 1);
}

/**
 * pinos_fd_manager_add:
 * @manager: a #PinosFdManager
 * @client: a client id
 * @id: an id
 * @object: a pointer to an object to manage
 * @notify: callback to free @object
 *
 * Associate @object with @id for @client. @object will be kept alive until a
 * pinos_fd_manager_remove() or pinos_fd_manager_remove_client() with
 * the same id and client is made.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_fd_manager_add (PinosFdManager *manager,
                      const gchar *client, guint32 id,
                      gpointer object, GDestroyNotify notify)
{
  PinosFdManagerPrivate *priv;
  ObjectId *oid;
  ClientIds *cids;

  g_return_val_if_fail (PINOS_IS_FD_MANAGER (manager), FALSE);
  g_return_val_if_fail (client != NULL, FALSE);
  g_return_val_if_fail (object != NULL, FALSE);

  priv = manager->priv;

  g_mutex_lock (&priv->lock);
  /* get the id */
  oid = g_hash_table_lookup (priv->object_ids, GINT_TO_POINTER (id));
  if (oid == NULL) {
    /* first time, create and add */
    oid = object_id_new (id, object, notify);
    g_hash_table_insert (priv->object_ids, GINT_TO_POINTER (id), oid);
  } else {
    /* existed, check if the same object and notify*/
    if (oid->obj != object || oid->notify != notify)
      goto wrong_object;
    /* increment refcount */
    oid->refcount++;
  }
  /* find the client and add the id */
  cids = g_hash_table_lookup (priv->client_ids, client);
  if (cids == NULL) {
    cids = client_ids_new (oid);
    g_hash_table_insert (priv->client_ids, g_strdup (client), cids);
  } else {
    /* add the object to the client */
    cids->ids = g_list_prepend (cids->ids, oid);
  }
  g_mutex_unlock (&priv->lock);

  return TRUE;

  /* ERRORS */
wrong_object:
  {
    g_warning ("wrong object");
    g_mutex_unlock (&priv->lock);
    return FALSE;
  }
}

/**
 * pinos_fd_manager_remove:
 * @manager: a #PinosFdManager
 * @client: a client id
 * @id: an id
 *
 * Remove the id associated with client from @manager.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_fd_manager_remove (PinosFdManager *manager,
                         const gchar *client, guint32 id)
{
  PinosFdManagerPrivate *priv;
  ObjectId *oid;
  ClientIds *cids;

  g_return_val_if_fail (PINOS_IS_FD_MANAGER (manager), FALSE);
  g_return_val_if_fail (client != NULL, FALSE);

  priv = manager->priv;

  g_mutex_lock (&priv->lock);
  oid = g_hash_table_lookup (priv->object_ids, GINT_TO_POINTER (id));
  if (oid) {
    cids = g_hash_table_lookup (priv->client_ids, client);
    if (cids) {
      GList *find = g_list_find (cids->ids, oid);

      if (find) {
        cids->ids = g_list_delete_link (cids->ids, find);
        oid->refcount--;
        if (oid->refcount == 0) {
          g_hash_table_remove (priv->object_ids, GINT_TO_POINTER (id));
        }
      }
    }
  }
  g_mutex_unlock (&priv->lock);

  return TRUE;
}

static void
remove_id (ObjectId *oid, PinosFdManager *manager)
{
  PinosFdManagerPrivate *priv = manager->priv;

  oid->refcount--;
  if (oid->refcount == 0) {
    g_hash_table_remove (priv->object_ids, GINT_TO_POINTER (oid->id));
  }
}

/**
 * pinos_fd_manager_remove_all:
 * @manager: a #PinosFdManager
 * @client: a client id
 *
 * Remove all ids from @manager associated with @client.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_fd_manager_remove_all (PinosFdManager *manager,
                             const gchar *client)
{
  PinosFdManagerPrivate *priv;
  ClientIds *cids;

  g_return_val_if_fail (PINOS_IS_FD_MANAGER (manager), FALSE);
  g_return_val_if_fail (client != NULL, FALSE);

  priv = manager->priv;

  g_mutex_lock (&priv->lock);
  cids = g_hash_table_lookup (priv->client_ids, client);
  if (cids) {
    g_list_foreach (cids->ids, (GFunc) remove_id, manager);
    g_hash_table_remove (priv->client_ids, client);
    client_ids_free (cids);
  }
  g_mutex_unlock (&priv->lock);

  return TRUE;
}

static void
pinos_fd_manager_finalize (GObject * object)
{
  PinosFdManager *mgr = PINOS_FD_MANAGER (object);
  PinosFdManagerPrivate *priv = mgr->priv;

  g_hash_table_unref (priv->object_ids);
  g_hash_table_unref (priv->client_ids);

  G_OBJECT_CLASS (pinos_fd_manager_parent_class)->finalize (object);
}

static void
pinos_fd_manager_init (PinosFdManager * mgr)
{
  PinosFdManagerPrivate *priv = mgr->priv = PINOS_FD_MANAGER_GET_PRIVATE (mgr);

  g_mutex_init (&priv->lock);
  priv->object_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) object_id_free);
  priv->client_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
pinos_fd_manager_class_init (PinosFdManagerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = pinos_fd_manager_finalize;

  g_type_class_add_private (klass, sizeof (PinosFdManagerPrivate));

  g_mutex_init (&manager_lock);
  managers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}
