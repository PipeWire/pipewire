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

#include <gio/gio.h>

#include <spa/include/spa/video/format.h>
#include <spa/include/spa/debug.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/link.h"
#include "pinos/server/port.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_LINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_LINK, PinosLinkPrivate))

struct _PinosLinkPrivate
{
  PinosDaemon *daemon;
  PinosLink1 *iface;

  gchar *object_path;

  gulong input_id, output_id;

  gboolean active;
  gboolean negotiated;
  gboolean allocated;
  gboolean started;

  PinosPort *output;
  PinosPort *input;

  SpaNode *output_node;
  uint32_t output_port;
  SpaNode *input_node;
  uint32_t input_port;

  SpaNodeState input_state;
  SpaNodeState output_state;

  SpaBuffer *in_buffers[16];
  unsigned int n_in_buffers;
  SpaBuffer *out_buffers[16];
  unsigned int n_out_buffers;
};

G_DEFINE_TYPE (PinosLink, pinos_link, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OUTPUT,
  PROP_INPUT,
  PROP_OBJECT_PATH,
};

enum
{
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static gboolean
on_output_buffer (PinosPort *port, uint32_t buffer_id, GError **error, gpointer user_data)
{
  PinosLink *link = user_data;
  PinosLinkPrivate *priv = link->priv;

  return pinos_port_receive_buffer (priv->input, buffer_id, error);
}

static gboolean
on_output_event (PinosPort *port, SpaEvent *event, GError **error, gpointer user_data)
{
  PinosLink *link = user_data;
  PinosLinkPrivate *priv = link->priv;

  return pinos_port_receive_event (priv->input, event, error);
}

static gboolean
on_input_buffer (PinosPort *port, uint32_t buffer_id, GError **error, gpointer user_data)
{
  PinosLink *link = user_data;
  PinosLinkPrivate *priv = link->priv;

  return pinos_port_receive_buffer (priv->output, buffer_id, error);
}

static gboolean
on_input_event (PinosPort *port, SpaEvent *event, GError **error, gpointer user_data)
{
  PinosLink *link = user_data;
  PinosLinkPrivate *priv = link->priv;

  return pinos_port_receive_event (priv->output, event, error);
}

static void
pinos_link_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosLink *link = PINOS_LINK (_object);
  PinosLinkPrivate *priv = link->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_OUTPUT:
      g_value_set_object (value, priv->output);
      break;

    case PROP_INPUT:
      g_value_set_object (value, priv->input);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (link, prop_id, pspec);
      break;
  }
}

static void
pinos_link_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosLink *link = PINOS_LINK (_object);
  PinosLinkPrivate *priv = link->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_OUTPUT:
      priv->output = g_value_get_object (value);
      priv->output_node = priv->output->node->node;
      priv->output_port = priv->output->id;
      break;

    case PROP_INPUT:
      priv->input = g_value_get_object (value);
      priv->input_node = priv->input->node->node;
      priv->input_port = priv->input->id;
      break;

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (link, prop_id, pspec);
      break;
  }
}

static void
link_register_object (PinosLink *link)
{
  PinosLinkPrivate *priv = link->priv;
  PinosObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf (PINOS_DBUS_OBJECT_LINK);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_link1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (priv->daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("link %p: register object %s", link, priv->object_path);
}

static void
link_unregister_object (PinosLink *link)
{
  PinosLinkPrivate *priv = link->priv;

  g_debug ("link %p: unregister object", link);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
}

static SpaResult
do_negotiate (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaResult res;
  SpaFormat *filter, *format;
  void *istate = NULL, *ostate = NULL;

  g_debug ("link %p: doing set format", this);

  priv->negotiated = TRUE;

again:
  if ((res = spa_node_port_enum_formats (priv->input_node,
                                         priv->input_port,
                                         &filter,
                                         NULL,
                                         &istate)) < 0) {
    g_warning ("error input enum formats: %d", res);
    goto error;
  }
  spa_debug_format (filter);

  if ((res = spa_node_port_enum_formats (priv->output_node,
                                         priv->output_port,
                                         &format,
                                         filter,
                                         &ostate)) < 0) {
    if (res == SPA_RESULT_ENUM_END) {
      ostate = NULL;
      goto again;
    }
    g_warning ("error output enum formats: %d", res);
    goto error;
  }
  spa_format_fixate (format);

  spa_debug_format (format);

  if ((res = spa_node_port_set_format (priv->output_node, priv->output_port, 0, format)) < 0) {
    g_warning ("error set format output: %d", res);
    goto error;
  }

  if ((res = spa_node_port_set_format (priv->input_node, priv->input_port, 0, format)) < 0) {
    g_warning ("error set format input: %d", res);
    goto error;
  }

  return SPA_RESULT_OK;

error:
  {
    priv->negotiated = FALSE;
    return res;
  }
}

static SpaResult
do_allocation (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaResult res;
  const SpaPortInfo *iinfo, *oinfo;
  SpaPortInfoFlags in_flags, out_flags;

  g_debug ("link %p: doing alloc buffers %p %p", this, priv->output_node, priv->input_node);
  /* find out what's possible */
  if ((res = spa_node_port_get_info (priv->output_node, priv->output_port, &oinfo)) < 0) {
    g_warning ("error get port info: %d", res);
    goto error;
  }
  if ((res = spa_node_port_get_info (priv->input_node, priv->input_port, &iinfo)) < 0) {
    g_warning ("error get port info: %d", res);
    goto error;
  }

  priv->allocated = TRUE;

  spa_debug_port_info (oinfo);
  spa_debug_port_info (iinfo);

  priv->n_in_buffers = 16;
  priv->n_out_buffers = 16;

  if ((oinfo->flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
      (iinfo->flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
    out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
  } else if ((oinfo->flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
      (iinfo->flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
    out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else if ((oinfo->flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
      (iinfo->flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
    out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

    if ((res = spa_buffer_alloc (oinfo->params, oinfo->n_params,
                                 priv->in_buffers,
                                 &priv->n_in_buffers)) < 0) {
      g_warning ("error alloc buffers: %d", res);
      goto error;
    }
    memcpy (priv->out_buffers, priv->in_buffers, priv->n_in_buffers * sizeof (SpaBuffer*));
    priv->n_out_buffers = priv->n_in_buffers;
  } else if ((oinfo->flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
      (iinfo->flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
    out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else {
    g_warning ("error no common allocation found");
    res = SPA_RESULT_ERROR;
    goto error;
  }

  if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
    if ((res = spa_node_port_alloc_buffers (priv->input_node, priv->input_port,
                                            oinfo->params, oinfo->n_params,
                                            priv->in_buffers, &priv->n_in_buffers)) < 0) {
      g_warning ("error alloc buffers: %d", res);
      goto error;
    }
    priv->input->n_buffers = priv->n_in_buffers;
    priv->input->buffers = priv->in_buffers;
  }
  if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
    if ((res = spa_node_port_alloc_buffers (priv->output_node, priv->output_port,
                                            iinfo->params, iinfo->n_params,
                                            priv->out_buffers, &priv->n_out_buffers)) < 0) {
      g_warning ("error alloc buffers: %d", res);
      goto error;
    }
    priv->output->n_buffers = priv->n_out_buffers;
    priv->output->buffers = priv->out_buffers;
  }
  if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    if ((res = spa_node_port_use_buffers (priv->input_node, priv->input_port,
                                          priv->out_buffers, priv->n_out_buffers)) < 0) {
      g_warning ("error use buffers: %d", res);
      goto error;
    }
    priv->input->n_buffers = priv->n_out_buffers;
    priv->input->buffers = priv->out_buffers;
  }
  if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    if ((res = spa_node_port_use_buffers (priv->output_node, priv->output_port,
                                          priv->in_buffers, priv->n_in_buffers)) < 0) {
      g_warning ("error use buffers: %d", res);
      goto error;
    }
    priv->output->n_buffers = priv->n_in_buffers;
    priv->output->buffers = priv->in_buffers;
  }

  return SPA_RESULT_OK;

error:
  {
    priv->allocated = FALSE;
    return res;
  }
}

static SpaResult
do_start (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaCommand cmd;
  SpaResult res;

  priv->started = TRUE;

  cmd.type = SPA_COMMAND_START;
  if ((res = spa_node_send_command (priv->input_node, &cmd)) < 0)
    g_warning ("got error %d", res);
  if ((res = spa_node_send_command (priv->output_node, &cmd)) < 0)
    g_warning ("got error %d", res);

  return res;
}

static SpaResult
do_pause (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaCommand cmd;
  SpaResult res;

  priv->started = FALSE;

  cmd.type = SPA_COMMAND_PAUSE;
  if ((res = spa_node_send_command (priv->input_node, &cmd)) < 0)
    g_warning ("got error %d", res);
  if ((res = spa_node_send_command (priv->output_node, &cmd)) < 0)
    g_warning ("got error %d", res);

  return res;
}

static SpaResult
check_states (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaResult res;

  g_debug ("link %p: input %d, output %d", this, priv->input_state, priv->output_state);

  if (priv->input_state >= SPA_NODE_STATE_CONFIGURE &&
      priv->output_state >= SPA_NODE_STATE_CONFIGURE &&
      !priv->negotiated) {
    if ((res = do_negotiate (this)) < 0)
      return res;
  }
  if (priv->input_state >= SPA_NODE_STATE_READY &&
      priv->output_state >= SPA_NODE_STATE_READY &&
      !priv->allocated) {
    if ((res = do_allocation (this)) < 0)
      return res;
  }
  if (priv->input_state >= SPA_NODE_STATE_PAUSED &&
      priv->output_state >= SPA_NODE_STATE_PAUSED &&
      !priv->started) {
    if ((res = do_start (this)) < 0)
      return res;
  }
  return SPA_RESULT_OK;
}

static void
on_node_state_notify (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  g_debug ("link %p: node %p state change", this, obj);
  if (obj == G_OBJECT (priv->input->node))
    priv->input_state = priv->input->node->node_state;
  else
    priv->output_state = priv->output->node->node_state;

  check_states (this);
}

static gboolean
on_activate (PinosPort *port, gpointer user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  if (priv->active)
    return TRUE;
  priv->active = TRUE;

  if (priv->input == port)
    pinos_port_activate (priv->output);
  else
    pinos_port_activate (priv->input);

  check_states (this);

  return TRUE;
}

static gboolean
on_deactivate (PinosPort *port, gpointer user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  if (!priv->active)
    return TRUE;
  priv->active = FALSE;

  if (priv->input == port)
    pinos_port_deactivate (priv->output);
  else
    pinos_port_deactivate (priv->input);

  do_pause (this);

  return TRUE;
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "output") == 0) {
    gchar *port = g_strdup_printf ("%s:%d", pinos_node_get_object_path (priv->output->node),
                                priv->output->id);
    pinos_link1_set_src_port (priv->iface, port);
    g_free (port);
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "input") == 0) {
    gchar *port = g_strdup_printf ("%s:%d", pinos_node_get_object_path (priv->input->node),
                                priv->input->id);
    pinos_link1_set_dest_port (priv->iface, port);
    g_free (port);
  }
}


static void
pinos_link_constructed (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);
  PinosLinkPrivate *priv = this->priv;

  priv->output_id = pinos_port_add_send_cb (priv->output,
                                 on_output_buffer,
                                 on_output_event,
                                 this,
                                 NULL);
  priv->input_id = pinos_port_add_send_cb (priv->input,
                                 on_input_buffer,
                                 on_input_event,
                                 this,
                                 NULL);

  priv->input_state = priv->input->node->node_state;
  priv->output_state = priv->output->node->node_state;

  g_signal_connect (priv->input->node, "notify::node-state", (GCallback) on_node_state_notify, this);
  g_signal_connect (priv->output->node, "notify::node-state", (GCallback) on_node_state_notify, this);

  g_signal_connect (priv->input, "activate", (GCallback) on_activate, this);
  g_signal_connect (priv->input, "deactivate", (GCallback) on_deactivate, this);
  g_signal_connect (priv->output, "activate", (GCallback) on_activate, this);
  g_signal_connect (priv->output, "deactivate", (GCallback) on_deactivate, this);

  g_signal_connect (this, "notify", (GCallback) on_property_notify, this);

  G_OBJECT_CLASS (pinos_link_parent_class)->constructed (object);

  on_property_notify (G_OBJECT (this), NULL, this);
  g_debug ("link %p: constructed", this);
  link_register_object (this);
}

static void
pinos_link_dispose (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);
  PinosLinkPrivate *priv = this->priv;

  g_debug ("link %p: dispose", this);

  g_signal_handlers_disconnect_by_data (priv->input, this);
  g_signal_handlers_disconnect_by_data (priv->output, this);
  g_signal_handlers_disconnect_by_data (priv->input->node, this);
  g_signal_handlers_disconnect_by_data (priv->output->node, this);

  pinos_port_remove_send_cb (priv->input, priv->input_id);
  pinos_port_remove_send_cb (priv->output, priv->output_id);
  if (priv->active) {
    priv->active = FALSE;
    pinos_port_deactivate (priv->input);
    pinos_port_deactivate (priv->output);
  }
  priv->input = NULL;
  priv->output = NULL;
  link_unregister_object (this);

  G_OBJECT_CLASS (pinos_link_parent_class)->dispose (object);
}

static void
pinos_link_finalize (GObject * object)
{
  PinosLink *link = PINOS_LINK (object);
  PinosLinkPrivate *priv = link->priv;

  g_debug ("link %p: finalize", link);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->object_path);

  G_OBJECT_CLASS (pinos_link_parent_class)->finalize (object);
}

static void
pinos_link_class_init (PinosLinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  //PinosLinkClass *link_class = PINOS_LINK_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosLinkPrivate));

  gobject_class->constructed = pinos_link_constructed;
  gobject_class->dispose = pinos_link_dispose;
  gobject_class->finalize = pinos_link_finalize;
  gobject_class->set_property = pinos_link_set_property;
  gobject_class->get_property = pinos_link_get_property;

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
                                   PROP_OUTPUT,
                                   g_param_spec_object ("output",
                                                        "Output",
                                                        "The output port",
                                                        PINOS_TYPE_PORT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_INPUT,
                                   g_param_spec_object ("input",
                                                        "Input",
                                                        "The input port",
                                                        PINOS_TYPE_PORT,
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
}

static void
pinos_link_init (PinosLink * link)
{
  PinosLinkPrivate *priv = link->priv = PINOS_LINK_GET_PRIVATE (link);

  priv->iface = pinos_link1_skeleton_new ();
  g_debug ("link %p: new", link);
}

PinosLink *
pinos_link_new (PinosDaemon *daemon,
                PinosPort   *output,
                PinosPort   *input)
{
  PinosLink *link;
  PinosPort *tmp;

  if (output->direction != PINOS_DIRECTION_OUTPUT) {
    tmp = output;
    output = input;
    input = tmp;
  }

  link = g_object_new (PINOS_TYPE_LINK,
                       "daemon", daemon,
                       "output", output,
                       "input", input,
                       NULL);

  return link;
}

/**
 * pinos_link_remove:
 * @link: a #PinosLink
 *
 * Trigger removal of @link
 */
void
pinos_link_remove (PinosLink *link)
{
  g_return_if_fail (PINOS_IS_LINK (link));

  g_debug ("link %p: remove", link);
  g_signal_emit (link, signals[SIGNAL_REMOVE], 0, NULL);
}

/**
 * pinos_link_get_object_path:
 * @link: a #PinosLink
 *
 * Get the object patch of @link
 *
 * Returns: the object path of @source.
 */
const gchar *
pinos_link_get_object_path (PinosLink *link)
{
  PinosLinkPrivate *priv;

  g_return_val_if_fail (PINOS_IS_LINK (link), NULL);
  priv = link->priv;

  return priv->object_path;
}
