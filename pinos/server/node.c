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
#include "pinos/server/rt-loop.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

typedef struct {
  PinosPort port;
  GPtrArray *links;
} NodePort;

static NodePort *
new_node_port (PinosNode *node, uint32_t port)
{
  NodePort *np;
  np = g_slice_new0 (NodePort);
  np->port.node = node;
  np->port.port = port;
  np->links = g_ptr_array_new ();
  return np;
}

static void
free_node_port (NodePort *np)
{
  g_ptr_array_free (np->links, TRUE);
  g_slice_free (NodePort, np);
}

static NodePort *
find_node_port (GList *ports, PinosNode *node, uint32_t port)
{
  GList *walk;
  for (walk = ports; walk; walk = g_list_next (walk)) {
    NodePort *np = walk->data;
    if (np->port.node == node && np->port.port == port)
      return np;
  }
  return NULL;
}

struct _PinosNodePrivate
{
  PinosDaemon *daemon;
  PinosNode1 *iface;

  PinosClient *client;
  gchar *object_path;
  gchar *name;

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

  PinosRTLoop *loop;

  SpaNodeEventAsyncComplete ac;
  uint32_t pending_state_seq;
  PinosNodeState pending_state;
};

G_DEFINE_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_CLIENT,
  PROP_RTLOOP,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_PROPERTIES,
  PROP_NODE,
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
    NodePort *p = (ports ? ports->data : NULL);

    if (p && i < n_input_ports && p->port.port == input_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_input_ports && input_port_ids[i] < p->port.port) || i < n_input_ports) {
      NodePort *np;
      g_debug ("node %p: input port added %d", node, input_port_ids[i]);

      np = new_node_port (node, input_port_ids[i]);
      priv->input_ports = g_list_insert_before (priv->input_ports, ports, np);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_ADDED], 0, PINOS_DIRECTION_INPUT);
      i++;
    } else if (p) {
      GList *next;
      g_debug ("node %p: input port removed %d", node, p->port.port);

      next = g_list_next (ports);
      priv->input_ports = g_list_delete_link (priv->input_ports, ports);
      ports = next;

      free_node_port (p);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_REMOVED], 0, PINOS_DIRECTION_INPUT);
    } else
      break;
  }

  i = 0;
  ports = priv->output_ports;
  while (true) {
    NodePort *p = (ports ? ports->data : NULL);

    if (p && i < n_output_ports && p->port.port == output_port_ids[i]) {
      i++;
      ports = g_list_next (ports);
    } else if ((p && i < n_output_ports && output_port_ids[i] < p->port.port) || i < n_output_ports) {
      NodePort *np;
      g_debug ("node %p: output port added %d", node, output_port_ids[i]);

      np = new_node_port (node, output_port_ids[i]);
      priv->output_ports = g_list_insert_before (priv->output_ports, ports, np);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_ADDED], 0, PINOS_DIRECTION_INPUT);
      i++;
    } else if (p) {
      GList *next;
      g_debug ("node %p: output port removed %d", node, p->port.port);

      next = g_list_next (ports);
      priv->output_ports = g_list_delete_link (priv->output_ports, ports);
      ports = next;

      free_node_port (p);

      if (!priv->async_init)
        g_signal_emit (node, signals[SIGNAL_PORT_REMOVED], 0, PINOS_DIRECTION_INPUT);
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
  cmd.data = NULL;
  cmd.size = 0;
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
  cmd.data = NULL;
  cmd.size = 0;
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
    NodePort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_INPUT, p->port.port, 0, NULL)) < 0)
      g_warning ("error unset format output: %d", res);
    p->port.buffers = NULL;
    p->port.n_buffers = 0;
    if (p->port.allocated)
      pinos_memblock_free (&p->port.buffer_mem);
    p->port.allocated = FALSE;
  }
  for (walk = priv->output_ports; walk; walk = g_list_next (walk)) {
    NodePort *p = walk->data;
    if ((res = spa_node_port_set_format (this->node, SPA_DIRECTION_OUTPUT, p->port.port, 0, NULL)) < 0)
      g_warning ("error unset format output: %d", res);
    p->port.buffers = NULL;
    p->port.n_buffers = 0;
    if (p->port.allocated)
      pinos_memblock_free (&p->port.buffer_mem);
    p->port.allocated = FALSE;
  }
  return res;
}

static void
send_clock_update (PinosNode *this)
{
  SpaNodeCommand cmd;
  SpaNodeCommandClockUpdate cu;
  SpaResult res;

  cmd.type = SPA_NODE_COMMAND_CLOCK_UPDATE;
  cmd.data = &cu;
  cmd.size = sizeof (cu);

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

  if ((res = spa_node_send_command (this->node, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static gboolean
node_set_state (PinosNode       *this,
                PinosNodeState   state)
{
  PinosNodePrivate *priv = this->priv;
  SpaResult res = SPA_RESULT_OK;

  g_debug ("node %p: set state %s", this, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_CREATING:
      return FALSE;

    case PINOS_NODE_STATE_SUSPENDED:
      res = suspend_node (this);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      res = pause_node (this);
      break;

    case PINOS_NODE_STATE_RUNNING:
      send_clock_update (this);
      res = start_node (this);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  if (SPA_RESULT_IS_ERROR (res))
    return FALSE;

  if (SPA_RESULT_IS_ASYNC (res)) {
    priv->pending_state_seq = SPA_RESULT_ASYNC_SEQ (res);
    priv->pending_state = state;
  } else {
    pinos_node_update_state (this, state);
  }

  return TRUE;
}

static gboolean
do_read_link (PinosNode *this, PinosLink *link)
{
  SpaRingbufferArea areas[2];
  SpaResult res;
  gboolean pushed = FALSE;

  spa_ringbuffer_get_read_areas (&link->ringbuffer, areas);

  if (areas[0].len > 0) {
    SpaPortInputInfo iinfo[1];

    if (link->in_ready <= 0)
      return FALSE;

    link->in_ready--;

    iinfo[0].port_id = link->input->port;
    iinfo[0].buffer_id = link->queue[areas[0].offset];
    iinfo[0].flags = SPA_PORT_INPUT_FLAG_NONE;

    if ((res = spa_node_port_push_input (link->input->node->node, 1, iinfo)) < 0)
      g_warning ("node %p: error pushing buffer: %d, %d", this, res, iinfo[0].status);
    else
      pushed = TRUE;

    spa_ringbuffer_read_advance (&link->ringbuffer, 1);
  }
  return pushed;
}

static void
do_handle_async_complete (PinosNode *this)
{
  PinosNodePrivate *priv = this->priv;
  SpaNodeEventAsyncComplete *ac = &priv->ac;

  g_debug ("node %p: async complete %u %d", this, ac->seq, ac->res);

  if (priv->async_init) {
    init_complete (this);
    priv->async_init = FALSE;
  }
  if (priv->pending_state_seq == ac->seq) {
    pinos_node_update_state (this, priv->pending_state);
  }
  g_signal_emit (this, signals[SIGNAL_ASYNC_COMPLETE], 0, ac->seq, ac->res);
}

static void
on_node_event (SpaNode *node, SpaNodeEvent *event, void *user_data)
{
  PinosNode *this = user_data;
  PinosNodePrivate *priv = this->priv;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
    case SPA_NODE_EVENT_TYPE_DRAINED:
    case SPA_NODE_EVENT_TYPE_MARKER:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
      break;

    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    {
      SpaNodeEventAsyncComplete *ac = event->data;

      priv->ac = *ac;
      g_main_context_invoke (NULL,
                            (GSourceFunc) do_handle_async_complete,
                            this);
      break;
    }

    case SPA_NODE_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *item = event->data;
      pinos_rtloop_add_poll (priv->loop, item);
      break;
    }
    case SPA_NODE_EVENT_TYPE_UPDATE_POLL:
    {
      SpaPollItem *item = event->data;
      pinos_rtloop_update_poll (priv->loop, item);
      break;
    }
    case SPA_NODE_EVENT_TYPE_REMOVE_POLL:
    {
      SpaPollItem *item = event->data;
      pinos_rtloop_remove_poll (priv->loop, item);
      break;
    }
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    {
      SpaNodeEventNeedInput *ni = event->data;
      NodePort *p;
      guint i;

      if (!(p = find_node_port (priv->input_ports, this, ni->port_id)))
        break;

      for (i = 0; i < p->links->len; i++) {
        PinosLink *link = g_ptr_array_index (p->links, i);

        link->in_ready++;
        do_read_link (this, link);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaNodeEventHaveOutput *ho = event->data;
      SpaPortOutputInfo oinfo[1] = { 0, };
      SpaResult res;
      gboolean pushed = FALSE;
      NodePort *p;
      guint i;

      oinfo[0].port_id = ho->port_id;

      if ((res = spa_node_port_pull_output (node, 1, oinfo)) < 0) {
        g_warning ("node %p: got pull error %d, %d", this, res, oinfo[0].status);
        break;
      }

      if (!(p = find_node_port (priv->output_ports, this, oinfo[0].port_id)))
        break;

      for (i = 0; i < p->links->len; i++) {
        PinosLink *link = g_ptr_array_index (p->links, i);
        SpaRingbufferArea areas[2];

        spa_ringbuffer_get_write_areas (&link->ringbuffer, areas);
        if (areas[0].len > 0) {
          link->queue[areas[0].offset] = oinfo[0].buffer_id;
          spa_ringbuffer_write_advance (&link->ringbuffer, 1);

          pushed = do_read_link (this, link);
        }
      }
      if (!pushed) {
        g_debug ("node %p: discarded buffer %u", this, oinfo[0].buffer_id);
        if ((res = spa_node_port_reuse_buffer (node, oinfo[0].port_id, oinfo[0].buffer_id)) < 0)
          g_warning ("node %p: error reuse buffer: %d", node, res);
      }
      break;
    }
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaResult res;
      SpaNodeEventReuseBuffer *rb = event->data;
      NodePort *p;
      guint i;

      if (!(p = find_node_port (priv->input_ports, this, rb->port_id)))
        break;

      for (i = 0; i < p->links->len; i++) {
        PinosLink *link = g_ptr_array_index (p->links, i);

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

    case PROP_RTLOOP:
      g_value_set_object (value, priv->loop);
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

    case PROP_RTLOOP:
    {
      SpaResult res;

      if (priv->loop)
        g_object_unref (priv->loop);
      priv->loop = g_value_dup_object (value);

      if (priv->loop) {
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
      void *iface;
      this->node = g_value_get_pointer (value);
      if (this->node->handle->get_interface (this->node->handle, SPA_INTERFACE_ID_CLOCK, &iface) >= 0)
        this->clock = iface;
      break;
    }
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
  }
  node_register_object (this);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  //PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: dispose", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  node_unregister_object (node);

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
  g_clear_object (&priv->loop);
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
                                   PROP_RTLOOP,
                                   g_param_spec_object ("rt-loop",
                                                        "RTLoop",
                                                        "The RTLoop",
                                                        PINOS_TYPE_RTLOOP,
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
                                             PINOS_TYPE_DIRECTION);
  signals[SIGNAL_PORT_REMOVED] = g_signal_new ("port-removed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_generic,
                                               G_TYPE_NONE,
                                               1,
                                               PINOS_TYPE_DIRECTION);
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
  priv->state = PINOS_NODE_STATE_CREATING;
  priv->pending_state_seq = SPA_ID_INVALID;
  pinos_node1_set_state (priv->iface, priv->state);
}

/**
 * pinos_node_new:
 * @daemon: a #PinosDaemon
 * @client: the client owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_node_new (PinosDaemon     *daemon,
                PinosClient     *client,
                const gchar     *name,
                PinosProperties *properties,
                SpaNode         *node)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  return g_object_new (PINOS_TYPE_NODE,
                       "daemon", daemon,
                       "client", client,
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

  g_debug ("node %p: remove", node);
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
guint
pinos_node_get_free_port (PinosNode       *node,
                          PinosDirection   direction)
{
  PinosNodePrivate *priv;
  guint free_port, n_ports, max_ports;
  GList *ports, *walk;

  g_return_val_if_fail (PINOS_IS_NODE (node), SPA_ID_INVALID);
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

    if (free_port < p->port)
      break;

    free_port = p->port + 1;
  }
  if (free_port >= max_ports && ports) {
    PinosPort *p = ports->data;

    free_port = p->port;
  } else
    return SPA_ID_INVALID;

  return free_port;

}

static void
do_remove_link (PinosLink *link, PinosNode *node)
{
  NodePort *p;
  PinosNode *n;

  if (link->output) {
    n = link->output->node;
    if ((p = find_node_port (n->priv->output_ports, n, link->output->port)))
      if (g_ptr_array_remove_fast (p->links, link))
        n->priv->n_used_output_links--;

    if (n->priv->n_used_output_links == 0 &&
        n->priv->n_used_input_links == 0)
      pinos_node_report_idle (n);
  }
  if (link->input->node) {
    n = link->input->node;
    if ((p = find_node_port (n->priv->input_ports, n, link->input->port)))
      if (g_ptr_array_remove_fast (p->links, link))
        n->priv->n_used_input_links--;

    if (n->priv->n_used_output_links == 0 &&
        n->priv->n_used_input_links == 0)
      pinos_node_report_idle (n);
  }
}

/**
 * pinos_node_link:
 * @output_node: a #PinosNode
 * @output_port: an output port
 * @input_node: a #PinosNode
 * @input_port: an input port
 * @format_filter: a format filter
 * @properties: extra properties
 * @error: an error or %NULL
 *
 * Make a link between @output_node and @input_node with the given ids
 *
 * If the ports were already linked, the existing links will be returned.
 *
 * If the output id was linked to a different input node or id, it
 * will be relinked.
 *
 * Returns: a new #PinosLink or %NULL and @error is set.
 */
PinosLink *
pinos_node_link (PinosNode       *output_node,
                 guint            output_port,
                 PinosNode       *input_node,
                 guint            input_port,
                 GPtrArray       *format_filter,
                 PinosProperties *properties,
                 GError         **error)
{
  PinosNodePrivate *priv;
  NodePort *onp, *inp;
  PinosLink *link = NULL;
  guint i;

  g_return_val_if_fail (PINOS_IS_NODE (output_node), NULL);
  g_return_val_if_fail (PINOS_IS_NODE (input_node), NULL);

  priv = output_node->priv;

  g_debug ("node %p: link %u %p:%u", output_node, output_port, input_node, input_port);

  if (output_node == input_node)
    goto same_node;

  onp = find_node_port (priv->output_ports, output_node, output_port);
  if (onp == NULL)
      goto no_output_ports;

  for (i = 0; i < onp->links->len; i++) {
    PinosLink *pl = g_ptr_array_index (onp->links, i);
    if (pl->input->node == input_node && pl->input->port == input_port) {
      link = pl;
      break;
    }
  }
  inp = find_node_port (input_node->priv->input_ports, input_node, input_port);
  if (inp == NULL)
      goto no_input_ports;

  if (link)  {
    /* FIXME */
    link->input->node = input_node;
    link->input->port = input_port;
    g_object_ref (link);
  } else {
    input_node->live = output_node->live;
    if (output_node->clock)
      input_node->clock = output_node->clock;
    g_debug ("node %p: clock %p", output_node, output_node->clock);

    link = g_object_new (PINOS_TYPE_LINK,
                       "daemon", priv->daemon,
                       "output-port", &onp->port,
                       "input-port", &inp->port,
                       "format-filter", format_filter,
                       "properties", properties,
                       NULL);

    g_ptr_array_add (onp->links, link);
    g_ptr_array_add (inp->links, link);

    g_signal_connect (link,
                      "remove",
                      (GCallback) do_remove_link,
                      output_node);

    output_node->priv->n_used_output_links++;
    input_node->priv->n_used_input_links++;
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
no_input_ports:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "can't get an input port to link to");
    return NULL;
  }
no_output_ports:
  {
    g_set_error (error,
                 PINOS_ERROR,
                 PINOS_ERROR_NODE_LINK,
                 "can't get an output port to link to");
    return NULL;
  }
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
  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  return NULL;
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
