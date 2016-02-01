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

struct _PinosContextPrivate
{
  GMainContext *context;

  gchar *name;
  PinosProperties *properties;

  guint id;
  GDBusConnection *connection;

  PinosContextFlags flags;

  PinosContextState state;
  GError *error;

  GDBusProxy *daemon;
  GDBusProxy *client;
  gboolean disconnecting;

  PinosSubscriptionFlags subscription_mask;
  PinosSubscribe *subscribe;

  GList *clients;
  GList *sources;
  GList *source_outputs;
};

void                   pinos_subscribe_get_proxy          (PinosSubscribe      *subscribe,
                                                           const gchar         *name,
                                                           const gchar         *object_path,
                                                           const gchar         *interface_name,
                                                           GCancellable        *cancellable,
                                                           GAsyncReadyCallback  callback,
                                                           gpointer             user_data);
GDBusProxy *           pinos_subscribe_get_proxy_finish   (PinosSubscribe *subscribe,
                                                           GAsyncResult   *res,
                                                           GError         **error);


typedef struct {
  guint32 version;
  guint32 length;
} PinosStackHeader;

typedef struct {
  gsize allocated_size;
  gsize size;
  gpointer data;
  GSocketControlMessage *message;
  gsize magic;
} PinosStackBuffer;

#define PSB(b)             ((PinosStackBuffer *) (b))
#define PSB_MAGIC          ((gsize) 5493683301u)
#define is_valid_buffer(b) (b != NULL && \
                            PSB(b)->magic == PSB_MAGIC)
