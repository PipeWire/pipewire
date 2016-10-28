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
#include <errno.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/node.h"
#include "pinos/server/data-loop.h"
#include "pinos/server/main-loop.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

static PinosPort *
new_pinos_port (PinosNode *node, PinosDirection direction, uint32_t port)
{
  PinosPort *np;
  np = g_slice_new0 (PinosPort);
  np->node = node;
  np->direction = direction;
  np->port = port;
  np->links = g_ptr_array_new ();
  return np;
}

static void
free_node_port (PinosPort *np)
{
  g_ptr_array_free (np->links, TRUE);
  g_slice_free (PinosPort, np);
}

struct _PinosNodePrivate
{
  PinosDaemon *daemon;
  PinosNode1 *iface;

  PinosClient *client;
  gchar *object_path;
  gchar *name;

  uint32_t seq;

  gboolean async_init;
  unsigned int max_input_ports;
  unsigned int max_output_ports;
  unsigned int n_input_ports;
  unsigned int n_output_ports;
  GList *input_ports;
  GList *output_ports;
  guint n_used_output_links;
  guint n_used_input_links;

  PinosNodeState state;
  GError *error;
  guint idle_timeout;

  PinosProperties *properties;

  PinosDataLoop *data_loop;
  PinosMainLoop *main_loop;

  struct {
    GPtrArray *links;
  } rt;
};

G_DEFINE_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_CLIENT,
  PROP_DATA_LOOP,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_PROPERTIES,
  PROP_NODE,
  PROP_CLOCK,
};

enum
{
  SIGNAL_REMOVE,
  SIGNAL_STATE_CHANGE,
  SIGNAL_PORT_ADDED,
  SIGNAL_PORT_REMOVED,
  SIGNAL_ASYNC_COMPLETE,
  LAST_SIGNAL
};

static void init_complete (PinosNode *this);

static guint signals[LAST_SIGNAL] = { 0 };

static void
update_port_ids (PinosNode *node, gboolean create)
{
  PinosNodePrivate *priv = node->priv;
  uint32_t *input_port_ids, *output_port_ids;
  guint n_input_ports, n_output_ports, max_input_ports, max_output_ports;
  guint i;
  GList *ports;

  if (node->node == NULL)
    return;

  spa_node_get_n_ports (node->node,
                        &n_input_ports,
                        &max_input_ports,
                        &n_output_ports,
                        &max_output_ports);

  input_port_ids = g_alloca (sizeof (uint32_t) * n_input_ports);
  output_port_ids = g_alloca (sizeof (uint32_t) * n_output_ports);

  spa_node_get_port_ids (node->node,
                         max_input_ports,
                         input_port_ids,
                         max_output_ports,
                         output_port_ids);

  g_debug ("node %p: update_port ids %u/%u, %u/%u", node,
      n_input_ports, max_input_ports, n_output_ports, max_output_ports);

  i = 0;
  ports = priv->input_ports;
  while (true) {
    PinosPort *p = (ports ? ports->data : NULL);

    if (p && i < n_input_ports && p->port == input_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_input_ports && input_port_ids[i] < p->port) || i < n_input_ports) {
      PinosPort *np;
      g_debug ("node %p: input port added %d", node, input_port_ids[i]);

      np = new_pinos_port (node, PINOS_DIRECTION_INPUT, input_port_ids[i]);
      priv->input_ports = g_list_insert_before (priv->input_ports, ports, np);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_ADDED], 0, np);
      i++;
    } else if (p) {
      GList *next;
      g_debug ("node %p: input port removed %d", node, p->port);

      next = g_list_next (ports);
      priv->input_ports = g_list_delete_link (priv->input_ports, ports);
      ports = next;

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_REMOVED], 0, p);

      free_node_port (p);
    } else
      break;
  }

  i = 0;
  ports = priv->output_ports;
  while (true) {
    PinosPort *p = (ports ? ports->data : NULL);

    if (p && i < n_output_ports && p->port == output_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_output_ports && output_port_ids[i] < p->port) || i < n_output_ports) {
      PinosPort *np;
      g_debug ("node %p: output port added %d", node, output_port_ids[i]);

      np = new_pinos_port (node, PINOS_DIRECTION_OUTPUT, output_port_ids[i]);
      priv->output_ports = g_list_insert_before (priv->output_ports, ports, np);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_ADDED], 0, np);
      i++;
    } else if (p) {
      GList *next;
      g_debug ("node %p: output port removed %d", node, p->port);

      next = g_list_next (ports);
      priv->output_ports = g_list_delete_link (priv->output_ports, ports);
      ports = next;

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_REMOVED], 0, p);

      free_node_port (p);
    } else
      break;
  }

  priv->max_input_ports = max_input_ports;
  priv->max_output_ports = max_output_ports;
  priv->n_input_ports = n_input_ports;
  priv->n_output_ports = n_output_ports;

  node->have_inputs = priv->n_input_ports > 0;
  node->have_outputs = priv->n_output_ports > 0;

}

static SpaResult
pause_node (PinosNode *this)
{
  SpaResult res;
  SpaNodeCommand cmd;

  g_debug ("node %p: pause node", this);

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  cmd.size = sizeof (cmd);
  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    g_debug ("got error %d", res);

  return res;
}

static SpaResult
start_node (PinosNode *this)
{
  SpaResult res;
  SpaNodeCommand cmd;

  g_debug ("node %p: start node", this);

  cmd.type = SPA_NODE_COMMAND_START;
  cmd.size = sizeof (cmd);
  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    g_debug ("got error %d", res);

  return res;
}

static SpaResult
suspend_node (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;
  SpaResult res = SPA_RESULT_OK;
  GList *walk;

  g_debug ("node %p: suspend node", this);

  for (walk = priv->input_ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_INPUT, p->port, 0, NULL)) < 0)
      g_warning ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = FALSE;
  }
  for (walk = priv->output_ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_OUTPUT, p->port, 0, NULL)) < 0)
      g_warning ("error unset format output: %d", res);
    p->buffers = NULL;
    p->n_buffers = 0;
    if (p->allocated)
      pinos_memblock_free (&p->buffer_mem);
    p->allocated = FALSE;
  }
  return res;
}

static void
send_clock_update (PinosNode *this)
{
  SpaNodeCommandClockUpdate cu;
  SpaResult res;

  cu.command.type = SPA_NODE_COMMAND_CLOCK_UPDATE;
  cu.command.size = sizeof (cu);
  cu.flags = 0;
  cu.change_mask = SPA_NODE_COMMAND_CLOCK_UPDATE_TIME |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_SCALE |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_STATE |
                   SPA_NODE_COMMAND_CLOCK_UPDATE_LATENCY;
  if (this->clock && this->live) {
    cu.flags = SPA_NODE_COMMAND_CLOCK_UPDATE_FLAG_LIVE;
    res = spa_clock_get_time (this->clock, &cu.rate, &cu.ticks, &cu.monotonic_time);
  } else {
    cu.rate = 1;
    cu.ticks = 0;
    cu.monotonic_time = 0;
  }
  cu.scale = (1 << 16) | 1;
  cu.state = SPA_CLOCK_STATE_RUNNING;

  if ((res = spa_node_send_command (this->node, &cu.command)) < 0)
    g_debug ("got error %d", res);
}

static SpaResult
do_read_link (SpaPoll        *poll,
              bool            async,
              uint32_t        seq,
              size_t          size,
              void           *data,
              void           *user_data)
{
  PinosNode *this = user_data;
  PinosLink *link = ((PinosLink**)data)[0];
  size_t offset;
  SpaResult res;

  if (link->input == NULL)
    return SPA_RESULT_OK;

  while (link->in_ready > 0 && spa_ringbuffer_get_read_offset (&link->ringbuffer, &offset) > 0) {
    SpaPortInputInfo iinfo[1];

    iinfo[0].port_id = link->input->port;
    iinfo[0].buffer_id = link->queue[offset];
    iinfo[0].flags = SPA_PORT_INPUT_FLAG_NONE;

    if ((res = spa_node_port_push_input (link->input->node->node, 1, iinfo)) < 0)
      g_warning ("node %p: error pushing buffer: %d, %d", this, res, iinfo[0].status);

    spa_ringbuffer_read_advance (&link->ringbuffer, 1);
    link->in_ready--;
  }
  return SPA_RESULT_OK;
}


static void
on_node_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
      break;

    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    {
      SpaNodeEventAsyncComplete *ac = (SpaNodeEventAsyncComplete *) event;

      g_debug ("node %p: async complete event %d %d", this, ac->seq, ac->res);
      if (!pinos_main_loop_defer_complete (priv->main_loop, this, ac->seq, ac->res))
        g_signal_emit (this, signals[SIGNAL_ASYNC_COMPLETE], 0, ac->seq, ac->res);
      break;
    }

    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaNodeEventNeedInput *ni = (SpaNodeEventNeedInput *) event;
      guint i;

      for (i = 0; i < priv->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (priv->rt.links, i);

        if (link->input == NULL || link->input->port != ni->port_id)
          continue;

        link->in_ready++;
        spa_poll_invoke (&link->input->node->priv->data_loop->poll,
                         do_read_link,
                         SPA_ID_INVALID,
                         sizeof (PinosLink *),
                         &link,
                         link->input->node);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaNodeEventHaveOutput *ho = (SpaNodeEventHaveOutput *) event;
      SpaPortOutputInfo oinfo[1] = { 0, };
      SpaResult res;
      guint i;
      gboolean pushed = FALSE;

      oinfo[0].port_id = ho->port_id;

      if ((res = spa_node_port_pull_output (node, 1, oinfo)) < 0) {
        g_warning ("node %p: got pull error %d, %d", this, res, oinfo[0].status);
        break;
      }

      for (i = 0; i < priv->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (priv->rt.links, i);
        PinosPort *output = link->output;
        PinosPort *input = link->input;
        size_t offset;

        if (output == NULL || input == NULL ||
            output->node->node != node || output->port != ho->port_id)
          continue;

        if (spa_ringbuffer_get_write_offset (&link->ringbuffer, &offset) > 0) {
          link->queue[offset] = oinfo[0].buffer_id;
          spa_ringbuffer_write_advance (&link->ringbuffer, 1);

          spa_poll_invoke (&link->input->node->priv->data_loop->poll,
                           do_read_link,
                           SPA_ID_INVALID,
                           sizeof (PinosLink *),
                           &link,
                           link->input->node);
          pushed = TRUE;
        }
      }
      if (!pushed) {
        if ((res = spa_node_port_reuse_buffer (node, oinfo[0].port_id, oinfo[0].buffer_id)) < 0)
          g_warning ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaResult res;
      SpaNodeEventReuseBuffer *rb = (SpaNodeEventReuseBuffer *) event;
      guint i;

      for (i = 0; i < priv->rt.links->len; i++) {
        PinosLink *link = g_ptr_array_index (priv->rt.links, i);

        if (link->input == NULL || link->input->port != rb->port_id || link->output == NULL)
          continue;

        if ((res = spa_node_port_reuse_buffer (link->output->node->node,
                                               link->output->port,
                                               rb->buffer_id)) < 0)
          g_warning ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      send_clock_update (this);
      break;
  }
}

static gboolean
handle_remove (PinosNode1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosNode *this = user_data;

  g_debug ("node %p: remove", this);
  pinos_node_remove (this);

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
  PinosNode *this = PINOS_NODE (_object);
  PinosNodePrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_CLIENT:
      g_value_set_object (value, priv->client);
      break;

    case PROP_DATA_LOOP:
      g_value_set_object (value, priv->data_loop);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_NODE:
      g_value_set_pointer (value, this->node);
      break;

    case PROP_CLOCK:
      g_value_set_pointer (value, this->clock);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
pinos_node_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PinosNode *this = PINOS_NODE (_object);
  PinosNodePrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_CLIENT:
      priv->client = g_value_get_object (value);
      break;

    case PROP_DATA_LOOP:
    {
      SpaResult res;

      if (priv->data_loop)
        g_object_unref (priv->data_loop);
      priv->data_loop = g_value_dup_object (value);

      if (priv->data_loop) {
        if ((res = spa_node_set_event_callback (this->node, on_node_event, this)) < 0)
         g_warning ("node %p: error setting callback", this);
      }
      break;
    }

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    case PROP_NODE:
    {
      this->node = g_value_get_pointer (value);
      break;
    }
    case PROP_CLOCK:
      this->clock = g_value_get_pointer (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
node_register_object (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);

  pinos_object_skeleton_set_node1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("node %p: register object %s", this, priv->object_path);
  pinos_daemon_add_node (daemon, this);

  return;
}

static void
node_unregister_object (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;

  g_debug ("node %p: unregister object %s", this, priv->object_path);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
  g_clear_pointer (&priv->object_path, g_free);
  pinos_daemon_remove_node (priv->daemon, this);
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "client") == 0) {
    if (priv->client)
      pinos_node1_set_owner (priv->iface, pinos_client_get_object_path (priv->client));
    else
      pinos_node1_set_owner (priv->iface, pinos_daemon_get_object_path (priv->daemon));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "name") == 0) {
    pinos_node1_set_name (priv->iface, pinos_node_get_name (this));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "properties") == 0) {
    PinosProperties *props = pinos_node_get_properties (this);
    pinos_node1_set_properties (priv->iface, props ? pinos_properties_to_variant (props) : NULL);
  }
}

static void
init_complete (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;

  update_port_ids (this, FALSE);
  g_debug ("node %p: init completed", this);
  priv->async_init = FALSE;
  on_property_notify (G_OBJECT (this), NULL, this);
  pinos_node_update_state (this, PINOS_NODE_STATE_SUSPENDED);
}

static void
pinos_node_constructed (GObject * obj)
{
  PinosNode *this = PINOS_NODE (obj);
  PinosNodePrivate *priv = this->priv;

  g_debug ("node %p: constructed", this);

  priv->main_loop = priv->daemon->main_loop;

  g_signal_connect (this, "notify", (GCallback) on_property_notify, this);
  G_OBJECT_CLASS (pinos_node_parent_class)->constructed (obj);

  if (this->node->info) {
    unsigned int i;

    if (priv->properties == NULL)
      priv->properties = pinos_properties_new (NULL, NULL);

    for (i = 0; i < this->node->info->n_items; i++)
      pinos_properties_set (priv->properties,
                            this->node->info->items[i].key,
                            this->node->info->items[i].value);
  }

  if (this->node->state > SPA_NODE_STATE_INIT) {
    init_complete (this);
  } else {
    priv->async_init = TRUE;
    pinos_main_loop_defer (priv->main_loop,
                           this,
                           SPA_RESULT_RETURN_ASYNC (0),
                           (PinosDeferFunc) init_complete,
                           NULL,
                           NULL);
  }
  node_register_object (this);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: dispose", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  node_unregister_object (node);

  pinos_main_loop_defer_cancel (priv->main_loop, node, 0);

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
  g_clear_object (&priv->data_loop);
  g_free (priv->name);
  g_clear_error (&priv->error);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_node_parent_class)->finalize (obj);
}

static void
pinos_node_class_init (PinosNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

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
                                   PROP_CLIENT,
                                   g_param_spec_object ("client",
                                                        "Client",
                                                        "The Client",
                                                        PINOS_TYPE_CLIENT,
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
                                   PROP_CLOCK,
                                   g_param_spec_pointer ("clock",
                                                         "Clock",
                                                         "The SPA clock",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_DATA_LOOP,
                                   g_param_spec_object ("data-loop",
                                                        "Data Loop",
                                                        "The Data Loop",
                                                        PINOS_TYPE_DATA_LOOP,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
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
  signals[SIGNAL_STATE_CHANGE] = g_signal_new ("state-change",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_generic,
                                               G_TYPE_NONE,
                                               2,
                                               PINOS_TYPE_NODE_STATE,
                                               PINOS_TYPE_NODE_STATE);
  signals[SIGNAL_PORT_ADDED] = g_signal_new ("port-added",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             1,
                                             G_TYPE_POINTER);
  signals[SIGNAL_PORT_REMOVED] = g_signal_new ("port-removed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_generic,
                                               G_TYPE_NONE,
                                               1,
                                               G_TYPE_POINTER);
  signals[SIGNAL_ASYNC_COMPLETE] = g_signal_new ("async-complete",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_generic,
                                               G_TYPE_NONE,
                                               2,
                                               G_TYPE_UINT,
                                               G_TYPE_UINT);
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
  priv->state = PINOS_NODE_STATE_CREATING;
  pinos_node1_set_state (priv->iface, priv->state);
  priv->rt.links = g_ptr_array_new_full (256, NULL);
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
 * pinos_node_get_client:
 * @node: a #PinosNode
 *
 * Get the owner client of @node.
 *
 * Returns: the owner client of @node.
 */
PinosClient *
pinos_node_get_client (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->client;
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

  if (node->flags & PINOS_NODE_FLAG_REMOVING)
    return;

  g_debug ("node %p: remove", node);
  node->flags |= PINOS_NODE_FLAG_REMOVING;
  g_signal_emit (node, signals[SIGNAL_REMOVE], 0, NULL);
}

/**
 * pinos_node_get_free_port:
 * @node: a #PinosNode
 * @direction: a #PinosDirection
 *
 * Find a new unused port id in @node with @direction
 *
 * Returns: the new port id or %SPA_ID_INVALID on error
 */
PinosPort *
pinos_node_get_free_port (PinosNode       *node,
                          PinosDirection   direction)
{
  PinosNodePrivate *priv;
  guint free_port, n_ports, max_ports;
  GList *ports, *walk;
  PinosPort *port = NULL;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  if (direction == PINOS_DIRECTION_INPUT) {
    max_ports = priv->max_input_ports;
    n_ports = priv->n_input_ports;
    ports = priv->input_ports;
  } else {
    max_ports = priv->max_output_ports;
    n_ports = priv->n_output_ports;
    ports = priv->output_ports;
  }
  free_port = 0;

  g_debug ("node %p: direction %d max %u, n %u, free_port %u", node, direction, max_ports, n_ports, free_port);

  for (walk = ports; walk; walk = g_list_next (walk)) {
    PinosPort *p = walk->data;

    if (free_port < p->port) {
      port = p;
      break;
    }
    free_port = p->port + 1;
  }
  if (free_port >= max_ports && ports) {
    port = ports->data;
  } else
    return NULL;

  return port;

}


static SpaResult
do_add_link (SpaPoll        *poll,
             bool            async,
             uint32_t        seq,
             size_t          size,
             void           *data,
             void           *user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;
  PinosLink *link = ((PinosLink**)data)[0];

  g_ptr_array_add (priv->rt.links, link);

  return SPA_RESULT_OK;
}

static PinosLink *
find_link (PinosPort *output_port, PinosPort *input_port)
{
  guint i;

  for (i = 0; i < output_port->links->len; i++) {
    PinosLink *pl = g_ptr_array_index (output_port->links, i);
    if (pl->input == input_port) {
      return pl;
    }
  }
  return NULL;
}

PinosLink *
pinos_port_get_link (PinosPort       *output_port,
                     PinosPort       *input_port)
{
  g_return_val_if_fail (output_port != NULL, NULL);
  g_return_val_if_fail (input_port != NULL, NULL);

  return find_link (output_port, input_port);
}

/**
 * pinos_port_link:
 * @output_port: an output port
 * @input_port: an input port
 * @format_filter: a format filter
 * @properties: extra properties
 * @error: an error or %NULL
 *
 * Make a link between @output_port and @input_port
 *
 * If the ports were already linked, the existing links will be returned.
 *
 * Returns: a new #PinosLink or %NULL and @error is set.
 */
PinosLink *
pinos_port_link (PinosPort       *output_port,
                 PinosPort       *input_port,
                 GPtrArray       *format_filter,
                 PinosProperties *properties,
                 GError         **error)
{
  PinosNodePrivate *priv;
  PinosNode *input_node, *output_node;
  PinosLink *link;

  g_return_val_if_fail (output_port != NULL, NULL);
  g_return_val_if_fail (input_port != NULL, NULL);

  output_node = output_port->node;
  priv = output_node->priv;
  input_node = input_port->node;

  g_debug ("port link %p:%u -> %p:%u", output_node, output_port->port, input_node, input_port->port);

  if (output_node == input_node)
    goto same_node;

  if (input_port->links->len > 0)
    goto was_linked;

  link = find_link (output_port, input_port);

  if (link)  {
    g_object_ref (link);
  } else {
    input_node->live = output_node->live;
    if (output_node->clock)
      input_node->clock = output_node->clock;
    g_debug ("node %p: clock %p, live %d", output_node, output_node->clock, output_node->live);

    link = g_object_new (PINOS_TYPE_LINK,
                       "daemon", priv->daemon,
                       "output-port", output_port,
                       "input-port", input_port,
                       "format-filter", format_filter,
                       "properties", properties,
                       NULL);

    g_ptr_array_add (output_port->links, link);
    g_ptr_array_add (input_port->links, link);

    output_node->priv->n_used_output_links++;
    input_node->priv->n_used_input_links++;

    spa_poll_invoke (&priv->data_loop->poll,
                     do_add_link,
                     SPA_ID_INVALID,
                     sizeof (PinosLink *),
                     &link,
                     output_node);
    spa_poll_invoke (&input_node->priv->data_loop->poll,
                     do_add_link,
                     SPA_ID_INVALID,
                     sizeof (PinosLink *),
                     &link,
                     input_node);
  }
  return link;

same_node:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "can't link a node to itself");
    return NULL;
  }
was_linked:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "input port was already linked");
    return NULL;
  }
}

static SpaResult
pinos_port_pause (PinosPort *port)
{
  SpaNodeCommand cmd;

  cmd.type = SPA_NODE_COMMAND_PAUSE;
  cmd.size = sizeof (cmd);
  return spa_node_port_send_command (port->node->node,
                                    port->direction,
                                    port->port,
                                    &cmd);
}

static SpaResult
do_remove_link_done (SpaPoll        *poll,
                     bool            async,
                     uint32_t        seq,
                     size_t          size,
                     void           *data,
                     void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodePrivate *priv = this->priv;
  PinosLink *link = ((PinosLink**)data)[0];

  g_debug ("port %p: finish unlink", port);
  if (port->direction == PINOS_DIRECTION_OUTPUT) {
    if (g_ptr_array_remove_fast (port->links, link))
      priv->n_used_output_links--;
    link->output = NULL;
  } else {
    if (g_ptr_array_remove_fast (port->links, link))
      priv->n_used_input_links--;
    link->input = NULL;
  }

  if (priv->n_used_output_links == 0 &&
      priv->n_used_input_links == 0) {
    pinos_node_report_idle (this);
  }

  if (!port->allocated) {
    g_debug ("port %p: clear buffers on port", port);
    spa_node_port_use_buffers (port->node->node,
                               port->direction,
                               port->port,
                               NULL, 0);
    port->buffers = NULL;
    port->n_buffers = 0;
  }

  pinos_main_loop_defer_complete (priv->main_loop,
                                  port,
                                  seq,
                                  SPA_RESULT_OK);
  g_object_unref (link);
  g_object_unref (port->node);

  return SPA_RESULT_OK;
}

static SpaResult
do_remove_link (SpaPoll        *poll,
                bool            async,
                uint32_t        seq,
                size_t          size,
                void           *data,
                void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodePrivate *priv = this->priv;
  PinosLink *link = ((PinosLink**)data)[0];
  SpaResult res;

  pinos_port_pause (port);

  g_ptr_array_remove_fast (priv->rt.links, link);

  res = spa_poll_invoke (&priv->main_loop->poll,
                         do_remove_link_done,
                         seq,
                         sizeof (PinosLink *),
                         &link,
                         port);
  return res;
}

SpaResult
pinos_port_unlink (PinosPort *port, PinosLink *link)
{
  SpaResult res;

  g_debug ("port %p: start unlink %p", port, link);

  g_object_ref (link);
  g_object_ref (port->node);
  res = spa_poll_invoke (&port->node->priv->data_loop->poll,
                         do_remove_link,
                         port->node->priv->seq++,
                         sizeof (PinosLink *),
                         &link,
                         port);
  return res;
}

static SpaResult
do_clear_buffers_done (SpaPoll        *poll,
                       bool            async,
                       uint32_t        seq,
                       size_t          size,
                       void           *data,
                       void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodePrivate *priv = this->priv;
  SpaResult res;

  g_debug ("port %p: clear buffers finish", port);

  res = spa_node_port_use_buffers (port->node->node,
                                   port->direction,
                                   port->port,
                                   NULL, 0);
  port->buffers = NULL;
  port->n_buffers = 0;

  pinos_main_loop_defer_complete (priv->main_loop,
                                  port,
                                  seq,
                                  res);
  return res;
}

static SpaResult
do_clear_buffers (SpaPoll        *poll,
                  bool            async,
                  uint32_t        seq,
                  size_t          size,
                  void           *data,
                  void           *user_data)
{
  PinosPort *port = user_data;
  PinosNode *this = port->node;
  PinosNodePrivate *priv = this->priv;
  SpaResult res;

  pinos_port_pause (port);

  res = spa_poll_invoke (&priv->main_loop->poll,
                         do_clear_buffers_done,
                         seq,
                         0, NULL,
                         port);
  return res;
}

SpaResult
pinos_port_clear_buffers (PinosPort *port)
{
  SpaResult res;

  g_debug ("port %p: clear buffers", port);
  res = spa_poll_invoke (&port->node->priv->data_loop->poll,
                         do_clear_buffers,
                         port->node->priv->seq++,
                         0, NULL,
                         port);
  return res;
}

/**
 * pinos_node_get_ports:
 * @node: a #PinosNode
 * @direction: a #PinosDirection
 *
 * Get the port in @node.
 *
 * Returns: a #GList of #PinosPort g_list_free after usage.
 */
GList *
pinos_node_get_ports (PinosNode *node, PinosDirection direction)
{
  GList *ports;
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  if (direction == PINOS_DIRECTION_INPUT) {
    ports = priv->input_ports;
  } else {
    ports = priv->output_ports;
  }
  return ports;
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

static void
on_state_complete (PinosNode *node,
                   gpointer   data,
                   SpaResult  res)
{
  PinosNodeState state = GPOINTER_TO_INT (data);

  if (SPA_RESULT_IS_ERROR (res)) {
    GError *error = NULL;
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_STATE,
                 "error changing node state: %d", res);
    pinos_node_report_error (node, error);
  } else
    pinos_node_update_state (node, state);
}

/**
 * pinos_node_set_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Set the state of @node to @state.
 *
 * Returns: a #SpaResult
 */
SpaResult
pinos_node_set_state (PinosNode      *node,
                      PinosNodeState  state)
{
  PinosNodePrivate *priv;
  SpaResult res = SPA_RESULT_OK;

  g_return_val_if_fail (PINOS_IS_NODE (node), SPA_RESULT_INVALID_ARGUMENTS);
  priv = node->priv;

  remove_idle_timeout (node);

  g_debug ("node %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_CREATING:
      return SPA_RESULT_ERROR;

    case PINOS_NODE_STATE_SUSPENDED:
      res = suspend_node (node);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      res = pause_node (node);
      break;

    case PINOS_NODE_STATE_RUNNING:
      send_clock_update (node);
      res = start_node (node);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  if (SPA_RESULT_IS_ERROR (res))
    return res;

  pinos_main_loop_defer (priv->main_loop,
                         node,
                         res,
                         (PinosDeferFunc) on_state_complete,
                         GINT_TO_POINTER (state),
                         NULL);

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
  PinosNodeState old;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  old = priv->state;
  if (old != state) {
    g_debug ("node %p: update state from %s -> %s", node,
        pinos_node_state_as_string (old),
        pinos_node_state_as_string (state));
    priv->state = state;
    pinos_node1_set_state (priv->iface, state);
    g_signal_emit (node, signals[SIGNAL_STATE_CHANGE], 0, old, priv->state);
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
  PinosNodeState old;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  old = priv->state;
  g_clear_error (&priv->error);
  remove_idle_timeout (node);
  priv->error = error;
  priv->state = PINOS_NODE_STATE_ERROR;
  g_debug ("node %p: got error state %s", node, error->message);
  pinos_node1_set_state (priv->iface, PINOS_NODE_STATE_ERROR);
  g_signal_emit (node, signals[SIGNAL_STATE_CHANGE], 0, old, priv->state);
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
