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

#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/eventfd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/node.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

struct _PinosNodePrivate
{
  PinosDaemon *daemon;
  PinosNode1 *iface;

  gchar *sender;
  gchar *object_path;
  gchar *name;

  unsigned int max_input_ports;
  unsigned int max_output_ports;
  unsigned int n_input_ports;
  unsigned int n_output_ports;
  uint32_t *input_port_ids;
  uint32_t *output_port_ids;

  PinosNodeState state;
  GError *error;
  guint idle_timeout;

  PinosProperties *properties;

  unsigned int n_poll;
  SpaPollItem poll[16];

  bool rebuild_fds;
  SpaPollFd fds[16];
  unsigned int n_fds;

  gboolean running;
  pthread_t thread;

  GHashTable *links;
};

G_DEFINE_ABSTRACT_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SENDER,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES,
  PROP_NODE,
  PROP_NODE_STATE,
};

enum
{
  SIGNAL_REMOVE,
  SIGNAL_PORT_ADDED,
  SIGNAL_PORT_REMOVED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static PinosDirection
get_port_direction (PinosNode *node, guint id)
{
  PinosNodePrivate *priv = node->priv;
  PinosDirection direction;

  direction = id < priv->max_input_ports ? PINOS_DIRECTION_INPUT : PINOS_DIRECTION_OUTPUT;

  return direction;
}

static void
update_port_ids (PinosNode *node, gboolean create)
{
  PinosNodePrivate *priv = node->priv;

  if (node->node == NULL)
    return;

  spa_node_get_n_ports (node->node,
                        &priv->n_input_ports,
                        &priv->max_input_ports,
                        &priv->n_output_ports,
                        &priv->max_output_ports);

  g_debug ("node %p: update_port ids %u, %u", node, priv->max_input_ports, priv->max_output_ports);

  priv->input_port_ids = g_realloc_n (priv->input_port_ids, priv->max_input_ports, sizeof (uint32_t));
  priv->output_port_ids = g_realloc_n (priv->output_port_ids, priv->max_output_ports, sizeof (uint32_t));

  spa_node_get_port_ids (node->node,
                         priv->max_input_ports,
                         priv->input_port_ids,
                         priv->max_output_ports,
                         priv->output_port_ids);
}

static void *
loop (void *user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;
  unsigned int i, j;

  g_debug ("node %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;
    unsigned int n_idle = 0;
    int r;

    /* prepare */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->idle_cb) {
        ndata.fds = NULL;
        ndata.n_fds = 0;
        ndata.user_data = p->user_data;
        p->idle_cb (&ndata);
        n_idle++;
      }
    }
    if (n_idle > 0)
      continue;

    /* rebuild */
    if (priv->rebuild_fds) {
      g_debug ("node %p: rebuild fds", this);
      priv->n_fds = 1;
      for (i = 0; i < priv->n_poll; i++) {
        SpaPollItem *p = &priv->poll[i];

        if (!p->enabled)
          continue;

        for (j = 0; j < p->n_fds; j++)
          priv->fds[priv->n_fds + j] = p->fds[j];
        p->fds = &priv->fds[priv->n_fds];
        priv->n_fds += p->n_fds;
      }
      priv->rebuild_fds = false;
    }

    /* before */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->before_cb) {
        ndata.fds = p->fds;
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        p->before_cb (&ndata);
      }
    }

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      g_debug ("node %p: select timeout", this);
      break;
    }

    /* check wakeup */
    if (priv->fds[0].revents & POLLIN) {
      uint64_t u;
      if (read (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
        g_warning ("node %p: failed to read fd", strerror (errno));
      continue;
    }

    /* after */
    for (i = 0; i < priv->n_poll; i++) {
      SpaPollItem *p = &priv->poll[i];

      if (p->enabled && p->after_cb) {
        ndata.fds = p->fds;
        ndata.n_fds = p->n_fds;
        ndata.user_data = p->user_data;
        p->after_cb (&ndata);
      }
    }
  }
  g_debug ("node %p: leave thread", this);

  return NULL;
}

static void
wakeup_thread (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;
  uint64_t u = 1;

  if (write (priv->fds[0].fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
    g_warning ("node %p: failed to write fd", strerror (errno));
}

static void
start_thread (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;
  int err;

  if (!priv->running) {
    priv->running = true;
    if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
      g_warning ("node %p: can't create thread", strerror (err));
      priv->running = false;
    }
  }
}

static void
stop_thread (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;

  if (priv->running) {
    priv->running = false;
    wakeup_thread (this);
    pthread_join (priv->thread, NULL);
  }
}

static void
pause_node (PinosNode *this)
{
  SpaResult res;
  SpaCommand cmd;

  g_debug ("node %p: pause node", this);

  cmd.type = SPA_COMMAND_PAUSE;
  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static void
suspend_node (PinosNode *this)
{
  SpaResult res;

  g_debug ("node %p: suspend node", this);

  if ((res = spa_node_port_set_format (this->node, 0, 0, NULL)) < 0)
    g_warning ("error unset format output: %d", res);

}

static gboolean
node_set_state (PinosNode       *this,
                PinosNodeState   state)
{
  g_debug ("node %p: set state %s", this, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      suspend_node (this);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      pause_node (this);
      break;

    case PINOS_NODE_STATE_RUNNING:
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (this, state);
  return TRUE;
}


static void
on_node_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;

  switch (event->type) {
    case SPA_EVENT_TYPE_PORT_ADDED:
    {
      SpaEventPortAdded *pa = event->data;

      update_port_ids (this, FALSE);

      g_signal_emit (this, signals[SIGNAL_PORT_ADDED], 0, get_port_direction (this, pa->port_id),
                                                          pa->port_id);
      break;
    }
    case SPA_EVENT_TYPE_PORT_REMOVED:
    {
      SpaEventPortRemoved *pr = event->data;

      update_port_ids (this, FALSE);

      g_signal_emit (this, signals[SIGNAL_PORT_REMOVED], 0, pr->port_id);
      break;
    }
    case SPA_EVENT_TYPE_STATE_CHANGE:
    {
      SpaEventStateChange *sc = event->data;

      g_debug ("node %p: update SPA state to %d", this, sc->state);
      g_object_notify (G_OBJECT (this), "node-state");

      if (sc->state == SPA_NODE_STATE_CONFIGURE) {
        update_port_ids (this, FALSE);
      }
      switch (sc->state) {
        case SPA_NODE_STATE_CONFIGURE:
        {
          GList *links, *walk;

          links = pinos_node_get_links (this);
          for (walk = links; walk; walk = g_list_next (walk)) {
            PinosLink *link = walk->data;
            pinos_link_activate (link);
          }
          g_list_free (links);
        }
        default:
          break;
      }
      break;
    }
    case SPA_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *poll = event->data;

      g_debug ("node %p: add poll %d", this, poll->n_fds);
      priv->poll[priv->n_poll] = *poll;
      priv->n_poll++;
      if (poll->n_fds)
        priv->rebuild_fds = true;
      wakeup_thread (this);

      start_thread (this);
      break;
    }
    case SPA_EVENT_TYPE_UPDATE_POLL:
    {
      unsigned int i;
      SpaPollItem *poll = event->data;

      for (i = 0; i < priv->n_poll; i++) {
        if (priv->poll[i].id == poll->id)
          priv->poll[i] = *poll;
      }
      if (poll->n_fds)
        priv->rebuild_fds = true;
      wakeup_thread (this);
      break;
    }
    case SPA_EVENT_TYPE_REMOVE_POLL:
    {
      SpaPollItem *poll = event->data;
      unsigned int i;

      g_debug ("node %p: remove poll %d", this, poll->n_fds);
      for (i = 0; i < priv->n_poll; i++) {
        if (priv->poll[i].id == poll->id) {
          priv->n_poll--;
          for (; i < priv->n_poll; i++)
            priv->poll[i] = priv->poll[i+1];
          break;
        }
      }
      if (priv->n_poll > 0) {
        priv->rebuild_fds = true;
        wakeup_thread (this);
      } else {
        stop_thread (this);
      }
      break;
    }
    case SPA_EVENT_TYPE_HAVE_OUTPUT:
    {
      PinosLink *link;
      SpaOutputInfo oinfo[1] = { 0, };
      SpaResult res;

      if ((res = spa_node_port_pull_output (node, 1, oinfo)) < 0)
        g_warning ("node %p: got pull error %d, %d", this, res, oinfo[0].status);

      link = g_hash_table_lookup (priv->links, GUINT_TO_POINTER (oinfo[0].port_id));
      if (link) {
        SpaInputInfo iinfo[1];

        iinfo[0].port_id = link->input_port;
        iinfo[0].buffer_id = oinfo[0].buffer_id;
        iinfo[0].flags = SPA_INPUT_FLAG_NONE;

        if ((res = spa_node_port_push_input (link->input_node->node, 1, iinfo)) < 0)
          g_warning ("node %p: error pushing buffer: %d, %d", this, res, iinfo[0].status);
      }
      break;
    }
    case SPA_EVENT_TYPE_REUSE_BUFFER:
    {
      PinosLink *link;
      SpaResult res;
      SpaEventReuseBuffer *rb = event->data;

      link = g_hash_table_lookup (priv->links, GUINT_TO_POINTER (rb->port_id));
      if (link) {
        if ((res = spa_node_port_reuse_buffer (link->output_node->node,
                                               link->output_port,
                                               rb->buffer_id)) < 0)
          g_warning ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    default:
      g_debug ("node %p: got event %d", this, event->type);
      break;
  }
}

static gboolean
handle_remove (PinosNode1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosNode *node = user_data;

  g_debug ("node %p: remove", node);
  pinos_node_remove (node);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
pinos_node_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PinosNode *node = PINOS_NODE (_object);
  PinosNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_SENDER:
      g_value_set_string (value, priv->sender);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_NODE:
      g_value_set_pointer (value, node->node);
      break;

    case PROP_NODE_STATE:
      g_value_set_uint (value, node->node->state);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_node_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PinosNode *node = PINOS_NODE (_object);
  PinosNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_SENDER:
      priv->sender = g_value_dup_string (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    case PROP_NODE:
      node->node = g_value_get_pointer (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
node_register_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);

  pinos_object_skeleton_set_node1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("node %p: register object %s", node, priv->object_path);
  pinos_daemon_add_node (daemon, node);

  return;
}

static void
node_unregister_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: unregister object %s", node, priv->object_path);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
  g_clear_pointer (&priv->object_path, g_free);
  pinos_daemon_remove_node (priv->daemon, node);
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosNodePrivate *priv = node->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "sender") == 0) {
    pinos_node1_set_owner (priv->iface, priv->sender);
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "name") == 0) {
    pinos_node1_set_name (priv->iface, pinos_node_get_name (node));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "properties") == 0) {
    PinosProperties *props = pinos_node_get_properties (node);
    pinos_node1_set_properties (priv->iface, props ? pinos_properties_to_variant (props) : NULL);
  }
}

static void
pinos_node_constructed (GObject * obj)
{
  PinosNode *this = PINOS_NODE (obj);
  PinosNodePrivate *priv = this->priv;
  SpaResult res;

  g_debug ("node %p: constructed", this);

  g_signal_connect (this, "notify", (GCallback) on_property_notify, this);
  G_OBJECT_CLASS (pinos_node_parent_class)->constructed (obj);

  priv->fds[0].fd = eventfd (0, 0);
  priv->fds[0].events = POLLIN | POLLPRI | POLLERR;
  priv->fds[0].revents = 0;
  priv->n_fds = 1;

  if ((res = spa_node_set_event_callback (this->node, on_node_event, this)) < 0)
    g_warning ("node %p: error setting callback", this);

  update_port_ids (this, TRUE);

  if (priv->sender == NULL) {
    priv->sender = g_strdup (pinos_daemon_get_sender (priv->daemon));
  }
  on_property_notify (G_OBJECT (this), NULL, this);

  node_register_object (this);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: dispose", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);
  stop_thread (node);

  node_unregister_object (node);

  g_hash_table_unref (priv->links);

  G_OBJECT_CLASS (pinos_node_parent_class)->dispose (obj);
}

static void
pinos_node_finalize (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: finalize", node);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->sender);
  g_free (priv->name);
  g_clear_error (&priv->error);
  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_free (priv->input_port_ids);
  g_free (priv->output_port_ids);

  G_OBJECT_CLASS (pinos_node_parent_class)->finalize (obj);
}

static void
pinos_node_class_init (PinosNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosNodePrivate));

  gobject_class->constructed = pinos_node_constructed;
  gobject_class->dispose = pinos_node_dispose;
  gobject_class->finalize = pinos_node_finalize;
  gobject_class->set_property = pinos_node_set_property;
  gobject_class->get_property = pinos_node_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SENDER,
                                   g_param_spec_string ("sender",
                                                        "Sender",
                                                        "The Sender",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The node name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state of the node",
                                                      PINOS_TYPE_NODE_STATE,
                                                      PINOS_NODE_STATE_SUSPENDED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the node",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NODE,
                                   g_param_spec_pointer ("node",
                                                         "Node",
                                                         "The SPA node",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NODE_STATE,
                                   g_param_spec_uint ("node-state",
                                                      "Node State",
                                                      "The state of the SPA node",
                                                      0,
                                                      G_MAXUINT,
                                                      SPA_NODE_STATE_INIT,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_REMOVE] = g_signal_new ("remove",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         g_cclosure_marshal_generic,
                                         G_TYPE_NONE,
                                         0,
                                         G_TYPE_NONE);
  signals[SIGNAL_PORT_ADDED] = g_signal_new ("port-added",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             2,
                                             PINOS_TYPE_DIRECTION,
                                             G_TYPE_UINT);
  signals[SIGNAL_PORT_REMOVED] = g_signal_new ("port-removed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_generic,
                                               G_TYPE_NONE,
                                               1,
                                               G_TYPE_UINT);

  node_class->set_state = node_set_state;
}

static void
pinos_node_init (PinosNode * node)
{
  PinosNodePrivate *priv = node->priv = PINOS_NODE_GET_PRIVATE (node);

  g_debug ("node %p: new", node);
  priv->iface = pinos_node1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-remove",
                                 (GCallback) handle_remove,
                                 node);
  priv->state = PINOS_NODE_STATE_SUSPENDED;
  pinos_node1_set_state (priv->iface, PINOS_NODE_STATE_SUSPENDED);

  priv->links = g_hash_table_new (g_direct_hash, g_direct_equal);
}

/**
 * pinos_node_new:
 * @daemon: a #PinosDaemon
 * @sender: the path of the owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_node_new (PinosDaemon     *daemon,
                const gchar     *sender,
                const gchar     *name,
                PinosProperties *properties,
                SpaNode         *node)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  return g_object_new (PINOS_TYPE_NODE,
                       "daemon", daemon,
                       "sender", sender,
                       "name", name,
                       "properties", properties,
                       "node", node,
                       NULL);
}

/**
 * pinos_node_get_name:
 * @node: a #PinosNode
 *
 * Get the name of @node
 *
 * Returns: the name of @node
 */
const gchar *
pinos_node_get_name (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->name;
}

/**
 * pinos_node_get_state:
 * @node: a #PinosNode
 *
 * Get the state of @node
 *
 * Returns: the state of @node
 */
PinosNodeState
pinos_node_get_state (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), PINOS_NODE_STATE_ERROR);
  priv = node->priv;

  return priv->state;
}

/**
 * pinos_node_get_properties:
 * @node: a #PinosNode
 *
 * Get the properties of @node
 *
 * Returns: the properties of @node
 */
PinosProperties *
pinos_node_get_properties (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->properties;
}

/**
 * pinos_node_get_daemon:
 * @node: a #PinosNode
 *
 * Get the daemon of @node.
 *
 * Returns: the daemon of @node.
 */
PinosDaemon *
pinos_node_get_daemon (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

 return priv->daemon;
}

/**
 * pinos_node_get_sender:
 * @node: a #PinosNode
 *
 * Get the owner path of @node.
 *
 * Returns: the owner path of @node.
 */
const gchar *
pinos_node_get_sender (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->sender;
}
/**
 * pinos_node_get_object_path:
 * @node: a #PinosNode
 *
 * Get the object path of @node.
 *
 * Returns: the object path of @node.
 */
const gchar *
pinos_node_get_object_path (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->object_path;
}

/**
 * pinos_node_remove:
 * @node: a #PinosNode
 *
 * Remove @node. This will stop the transfer on the node and
 * free the resources allocated by @node.
 */
void
pinos_node_remove (PinosNode *node)
{
  g_return_if_fail (PINOS_IS_NODE (node));

  g_debug ("node %p: remove", node);
  g_signal_emit (node, signals[SIGNAL_REMOVE], 0, NULL);
}

/**
 * pinos_node_get_free_port_id:
 * @node: a #PinosNode
 * @direction: a #PinosDirection
 *
 * Find a new unused port id in @node with @direction
 *
 * Returns: the new port id of %SPA_INVALID_ID on error
 */
guint
pinos_node_get_free_port_id (PinosNode       *node,
                             PinosDirection   direction)
{
  PinosNodePrivate *priv;
  guint i, free_port = 0, n_ports, max_ports;
  uint32_t *ports;

  g_return_val_if_fail (PINOS_IS_NODE (node), -1);
  priv = node->priv;

  if (direction == PINOS_DIRECTION_INPUT) {
    max_ports = priv->max_input_ports;
    n_ports = priv->n_input_ports;
    ports = priv->input_port_ids;
  } else {
    max_ports = priv->max_output_ports;
    n_ports = priv->n_output_ports;
    ports = priv->output_port_ids;
  }

  g_debug ("node %p: direction %d max %u, n %u", node, direction, max_ports, n_ports);

  for (i = 0; i < n_ports; i++) {
    if (free_port < ports[i])
      break;

    if (g_hash_table_lookup (priv->links, GUINT_TO_POINTER (free_port)) == NULL && free_port < max_ports)
      return free_port;

    free_port = ports[i] + 1;
  }
  if (free_port >= max_ports)
    return -1;

  return free_port;
}

static void
do_remove_link (PinosLink *link, PinosNode *node)
{
  g_hash_table_remove (link->output_node->priv->links, GUINT_TO_POINTER (link->output_port));
  if (g_hash_table_size (link->output_node->priv->links) == 0)
    pinos_node_report_idle (link->output_node);

  g_hash_table_remove (link->input_node->priv->links, GUINT_TO_POINTER (link->input_port));
  if (g_hash_table_size (link->input_node->priv->links) == 0)
    pinos_node_report_idle (link->input_node);
}

/**
 * pinos_node_link:
 * @output_node: a #PinosNode
 * @output_port: a port
 * @input_node: a #PinosNode
 * @input_port: a port
 * @format_filter: a format filter
 * @properties: extra properties
 *
 * Make a link between @output_node and @input_node on the given ports.
 *
 * If the ports were already linked, the existing linke will be returned.
 *
 * If the source port was linked to a different destination node or port, it
 * will be relinked.
 *
 * Returns: a new #PinosLink
 */
PinosLink *
pinos_node_link (PinosNode       *output_node,
                 guint            output_port,
                 PinosNode       *input_node,
                 guint            input_port,
                 GPtrArray       *format_filter,
                 PinosProperties *properties)
{
  PinosNodePrivate *priv;
  PinosLink *link;

  g_return_val_if_fail (PINOS_IS_NODE (output_node), NULL);
  g_return_val_if_fail (PINOS_IS_NODE (input_node), NULL);

  if (get_port_direction (output_node, output_port) != PINOS_DIRECTION_OUTPUT) {
    PinosNode *tmp;
    guint tmp_port;

    tmp = output_node;
    output_node = input_node;
    input_node = tmp;

    tmp_port = output_port;
    output_port = input_port;
    input_port = tmp_port;
  }

  priv = output_node->priv;

  link = g_hash_table_lookup (priv->links, GUINT_TO_POINTER (output_port));
  if (link) {
    link->input_node = input_node;
    link->input_port = input_port;
    g_object_ref (link);
  } else {

    link = g_object_new (PINOS_TYPE_LINK,
                        "daemon", priv->daemon,
                        "output-node", output_node,
                        "output-port", output_port,
                        "input-node", input_node,
                        "input-port", input_port,
                        "format-filter", format_filter,
                        "properties", properties,
                        NULL);

    g_signal_connect (link,
                      "remove",
                      (GCallback) do_remove_link,
                      output_node);

    g_hash_table_insert (priv->links, GUINT_TO_POINTER (output_port), link);
    g_hash_table_insert (input_node->priv->links, GUINT_TO_POINTER (input_port), link);
  }
  return link;
}

/**
 * pinos_node_get_links:
 * @node: a #PinosNode
 *
 * Get the links in @node.
 *
 * Returns: a #GList of #PinosLink g_list_free after usage.
 */
GList *
pinos_node_get_links (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return g_hash_table_get_values (priv->links);
}

static void
remove_idle_timeout (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  if (priv->idle_timeout) {
    g_source_remove (priv->idle_timeout);
    priv->idle_timeout = 0;
  }
}

/**
 * pinos_node_set_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Set the state of @node to @state.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_node_set_state (PinosNode      *node,
                      PinosNodeState  state)
{
  PinosNodeClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_NODE (node), FALSE);

  klass = PINOS_NODE_GET_CLASS (node);

  remove_idle_timeout (node);

  g_debug ("node %p: set state to %s", node, pinos_node_state_as_string (state));
  if (klass->set_state)
    res = klass->set_state (node, state);
  else
    res = FALSE;

  return res;
}

/**
 * pinos_node_update_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Update the state of a node. This method is used from
 * inside @node itself.
 */
void
pinos_node_update_state (PinosNode      *node,
                         PinosNodeState  state)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  if (priv->state != state) {
    g_debug ("node %p: update state to %s", node, pinos_node_state_as_string (state));
    priv->state = state;
    pinos_node1_set_state (priv->iface, state);
    g_object_notify (G_OBJECT (node), "state");
  }
}

/**
 * pinos_node_report_error:
 * @node: a #PinosNode
 * @error: a #GError
 *
 * Report an error from within @node.
 */
void
pinos_node_report_error (PinosNode *node,
                         GError    *error)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  g_clear_error (&priv->error);
  remove_idle_timeout (node);
  priv->error = error;
  priv->state = PINOS_NODE_STATE_ERROR;
  g_debug ("node %p: got error state %s", node, error->message);
  g_object_notify (G_OBJECT (node), "state");
}

static gboolean
idle_timeout (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  priv->idle_timeout = 0;
  g_debug ("node %p: idle timeout", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  return G_SOURCE_REMOVE;
}

/**
 * pinos_node_report_idle:
 * @node: a #PinosNode
 *
 * Mark @node as being idle. This will start a timeout that will
 * set the node to SUSPENDED.
 */
void
pinos_node_report_idle (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  g_debug ("node %p: report idle", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_IDLE);

  priv->idle_timeout = g_timeout_add_seconds (3,
                                              (GSourceFunc) idle_timeout,
                                              node);
}

/**
 * pinos_node_report_busy:
 * @node: a #PinosNode
 *
 * Mark @node as being busy. This will set the state of the node
 * to the RUNNING state.
 */
void
pinos_node_report_busy (PinosNode *node)
{
  g_return_if_fail (PINOS_IS_NODE (node));

  g_debug ("node %p: report busy", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_RUNNING);
}
