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

#include "pinos/dbus/org-pinos.h"

#define PINOS_LINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_LINK, PinosLinkPrivate))

struct _PinosLinkPrivate
{
  PinosDaemon *daemon;
  PinosLink1 *iface;

  gchar *object_path;
  GPtrArray *format_filter;
  PinosProperties *properties;

  PinosLinkState state;
  GError *error;

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
  PROP_OBJECT_PATH,
  PROP_OUTPUT_NODE,
  PROP_OUTPUT_ID,
  PROP_OUTPUT_PORT,
  PROP_INPUT_NODE,
  PROP_INPUT_ID,
  PROP_INPUT_PORT,
  PROP_FORMAT_FILTER,
  PROP_PROPERTIES,
  PROP_STATE,
};

enum
{
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_link_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosLink *this = PINOS_LINK (_object);
  PinosLinkPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_OUTPUT_NODE:
      g_value_set_object (value, this->output_node);
      break;

    case PROP_OUTPUT_ID:
      g_value_set_uint (value, this->output_id);
      break;

    case PROP_OUTPUT_PORT:
      g_value_set_uint (value, this->output_port);
      break;

    case PROP_INPUT_NODE:
      g_value_set_object (value, this->input_node);
      break;

    case PROP_INPUT_ID:
      g_value_set_uint (value, this->input_id);
      break;

    case PROP_INPUT_PORT:
      g_value_set_uint (value, this->input_port);
      break;

    case PROP_FORMAT_FILTER:
      g_value_set_boxed (value, priv->format_filter);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
pinos_link_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosLink *this = PINOS_LINK (_object);
  PinosLinkPrivate *priv = this->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_OUTPUT_NODE:
      this->output_node = g_value_dup_object (value);
      break;

    case PROP_OUTPUT_ID:
      this->output_id = g_value_get_uint (value);
      break;

    case PROP_OUTPUT_PORT:
      this->output_port = g_value_get_uint (value);
      break;

    case PROP_INPUT_NODE:
      this->input_node = g_value_dup_object (value);
      break;

    case PROP_INPUT_ID:
      this->input_id = g_value_get_uint (value);
      break;

    case PROP_INPUT_PORT:
      this->input_port = g_value_get_uint (value);
      break;

    case PROP_FORMAT_FILTER:
      priv->format_filter = g_value_dup_boxed (value);
      break;

    case PROP_PROPERTIES:
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (this, prop_id, pspec);
      break;
  }
}

static void
link_register_object (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  PinosObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf (PINOS_DBUS_OBJECT_LINK);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_link1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (priv->daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("link %p: register object %s", this, priv->object_path);
}

static void
link_unregister_object (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;

  g_debug ("link %p: unregister object", this);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
}

static void
pinos_link_update_state (PinosLink *link, PinosLinkState state)
{
  PinosLinkPrivate *priv = link->priv;

  if (state != priv->state) {
    g_clear_error (&priv->error);
    priv->state = state;
    g_debug ("link %p: got state %s", link, pinos_link_state_as_string (state));
    g_object_notify (G_OBJECT (link), "state");
  }
}

static void
pinos_link_report_error (PinosLink *link, GError *error)
{
  PinosLinkPrivate *priv = link->priv;

  g_clear_error (&priv->error);
  priv->error = error;
  priv->state = PINOS_LINK_STATE_ERROR;
  g_debug ("link %p: got error state %s", link, error->message);
  g_object_notify (G_OBJECT (link), "state");
}

static SpaResult
do_negotiate (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  SpaResult res;
  SpaFormat *filter = NULL, *format;
  void *istate = NULL, *ostate = NULL;
  GError *error = NULL;

  if (in_state != SPA_NODE_STATE_CONFIGURE && out_state != SPA_NODE_STATE_CONFIGURE)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_NEGOTIATING);

  /* both ports need a format */
  if (in_state == SPA_NODE_STATE_CONFIGURE && out_state == SPA_NODE_STATE_CONFIGURE) {
    g_debug ("link %p: doing negotiate format", this);
again:
    if ((res = spa_node_port_enum_formats (this->input_node->node,
                                           this->input_port,
                                           &filter,
                                           NULL,
                                           &istate)) < 0) {
      if (res == SPA_RESULT_ENUM_END && istate != NULL) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_FORMAT_NEGOTIATION,
                     "error input enum formats: %d", res);
        goto error;
      }
    }
    spa_debug_format (filter);

    if ((res = spa_node_port_enum_formats (this->output_node->node,
                                           this->output_port,
                                           &format,
                                           filter,
                                           &ostate)) < 0) {
      if (res == SPA_RESULT_ENUM_END) {
        ostate = NULL;
        goto again;
      }
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error output enum formats: %d", res);
      goto error;
    }
    spa_debug_format (format);
    spa_format_fixate (format);
  } else if (in_state == SPA_NODE_STATE_CONFIGURE && out_state > SPA_NODE_STATE_CONFIGURE) {
    /* only input needs format */
    if ((res = spa_node_port_get_format (this->output_node->node,
                                         this->output_port,
                                         (const SpaFormat **)&format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error get output format: %d", res);
      goto error;
    }
  } else if (out_state == SPA_NODE_STATE_CONFIGURE && in_state > SPA_NODE_STATE_CONFIGURE) {
    /* only output needs format */
    if ((res = spa_node_port_get_format (this->input_node->node,
                                         this->input_port,
                                         (const SpaFormat **)&format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error get input format: %d", res);
      goto error;
    }
  } else
    return SPA_RESULT_OK;

  g_debug ("link %p: doing set format", this);
  spa_debug_format (format);

  if (out_state == SPA_NODE_STATE_CONFIGURE) {
    g_debug ("link %p: doing set format on output", this);
    if ((res = spa_node_port_set_format (this->output_node->node,
                                         this->output_port,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error set output format: %d", res);
      goto error;
    }
  } else if (in_state == SPA_NODE_STATE_CONFIGURE) {
    g_debug ("link %p: doing set format on input", this);
    if ((res = spa_node_port_set_format (this->input_node->node,
                                         this->input_port,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error set input format: %d", res);
      goto error;
    }
  }
  return SPA_RESULT_OK;

error:
  {
    pinos_link_report_error (this, error);
    return res;
  }
}

static SpaResult
do_allocation (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  PinosLinkPrivate *priv = this->priv;
  SpaResult res;
  const SpaPortInfo *iinfo, *oinfo;
  SpaPortInfoFlags in_flags, out_flags;
  GError *error = NULL;

  if (in_state != SPA_NODE_STATE_READY && out_state != SPA_NODE_STATE_READY)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_ALLOCATING);

  g_debug ("link %p: doing alloc buffers %p %p", this, this->output_node, this->input_node);
  /* find out what's possible */
  if ((res = spa_node_port_get_info (this->output_node->node, this->output_port, &oinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get output port info: %d", res);
    goto error;
  }
  if ((res = spa_node_port_get_info (this->input_node->node, this->input_port, &iinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get input port info: %d", res);
    goto error;
  }
  in_flags = iinfo->flags;
  out_flags = oinfo->flags;

  if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
    this->output_node->live = true;
    this->input_node->live = true;
  }

  if (in_state == SPA_NODE_STATE_READY && out_state == SPA_NODE_STATE_READY) {
    if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
      priv->n_in_buffers = 16;
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

      if ((res = spa_buffer_alloc (oinfo->params, oinfo->n_params,
                                   priv->in_buffers,
                                   &priv->n_in_buffers)) < 0) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_BUFFER_ALLOCATION,
                     "error buffer alloc: %d", res);
        goto error;
      }
      memcpy (priv->out_buffers, priv->in_buffers, priv->n_in_buffers * sizeof (SpaBuffer*));
      priv->n_out_buffers = priv->n_in_buffers;
      g_debug ("allocated out_buffers %p, in_buffers %p", priv->out_buffers, priv->in_buffers);
    } else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
        (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
      out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
    } else {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "no common buffer alloc found");
      res = SPA_RESULT_ERROR;
      goto error;
    }
  } else if (in_state == SPA_NODE_STATE_READY && out_state > SPA_NODE_STATE_READY) {
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else if (out_state == SPA_NODE_STATE_READY && in_state > SPA_NODE_STATE_READY) {
    in_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    out_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  } else
    return SPA_RESULT_OK;

  spa_debug_port_info (oinfo);
  spa_debug_port_info (iinfo);

  if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
    priv->n_in_buffers = 16;
    if ((res = spa_node_port_alloc_buffers (this->input_node->node, this->input_port,
                                            oinfo->params, oinfo->n_params,
                                            priv->in_buffers, &priv->n_in_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error alloc input buffers: %d", res);
      goto error;
    }
    g_debug ("allocated in_buffers %p", priv->in_buffers);
  }
  else if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
    priv->n_out_buffers = 16;
    if ((res = spa_node_port_alloc_buffers (this->output_node->node, this->output_port,
                                            iinfo->params, iinfo->n_params,
                                            priv->out_buffers, &priv->n_out_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error alloc output buffers: %d", res);
      goto error;
    }
    g_debug ("allocated out_buffers %p", priv->out_buffers);
  }
  if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    g_debug ("using out_buffers %p", priv->out_buffers);
    if ((res = spa_node_port_use_buffers (this->input_node->node, this->input_port,
                                          priv->out_buffers, priv->n_out_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use input buffers: %d", res);
      goto error;
    }
  }
  else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    g_debug ("using in_buffers %p", priv->out_buffers);
    if ((res = spa_node_port_use_buffers (this->output_node->node, this->output_port,
                                          priv->in_buffers, priv->n_in_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use output buffers: %d", res);
      goto error;
    }
  } else {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "no common buffer alloc found");
      goto error;
  }

  return SPA_RESULT_OK;

error:
  {
    pinos_link_report_error (this, error);
    return res;
  }
}

static SpaResult
do_start (PinosLink *this, SpaNodeState in_state, SpaNodeState out_state)
{
  SpaResult res = SPA_RESULT_OK;

  if (in_state < SPA_NODE_STATE_PAUSED || out_state < SPA_NODE_STATE_PAUSED)
    return SPA_RESULT_OK;

  pinos_link_update_state (this, PINOS_LINK_STATE_PAUSED);

  if (in_state == SPA_NODE_STATE_PAUSED)
    pinos_node_set_state (this->input_node, PINOS_NODE_STATE_RUNNING);

  if (out_state == SPA_NODE_STATE_PAUSED)
    pinos_node_set_state (this->output_node, PINOS_NODE_STATE_RUNNING);

  pinos_link_update_state (this, PINOS_LINK_STATE_RUNNING);

  return res;
}

static SpaResult
check_states (PinosLink *this)
{
  SpaResult res;
  SpaNodeState in_state, out_state;

  in_state = this->input_node->node->state;
  out_state = this->output_node->node->state;

  g_debug ("link %p: input state %d, output state %d", this, in_state, out_state);

  if ((res = do_negotiate (this, in_state, out_state)) < 0)
    return res;

  if ((res = do_allocation (this, in_state, out_state)) < 0)
    return res;

  if ((res = do_start (this, in_state, out_state)) < 0)
    return res;

  return SPA_RESULT_OK;
}

static void
on_node_state_notify (GObject    *obj,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
  PinosLink *this = user_data;

  g_debug ("link %p: node %p state change", this, obj);
  check_states (this);
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "output-node") == 0) {
    pinos_link1_set_output_node (priv->iface, pinos_node_get_object_path (this->output_node));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "output-port") == 0) {
    pinos_link1_set_output_port (priv->iface, this->output_port);
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "input-node") == 0) {
    pinos_link1_set_input_node (priv->iface, pinos_node_get_object_path (this->input_node));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "input-port") == 0) {
    pinos_link1_set_input_port (priv->iface, this->input_port);
  }
}

static void
on_node_remove (PinosNode *node, PinosLink *this)
{
  g_signal_handlers_disconnect_by_data (node, this);
  if (node == this->input_node)
    g_clear_object (&this->input_node);
  else
    g_clear_object (&this->output_node);

  pinos_link_update_state (this, PINOS_LINK_STATE_UNLINKED);
}

static void
pinos_link_constructed (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);

  g_signal_connect (this->input_node, "remove", (GCallback) on_node_remove, this);
  g_signal_connect (this->output_node, "remove", (GCallback) on_node_remove, this);

  g_signal_connect (this->input_node, "notify::node-state", (GCallback) on_node_state_notify, this);
  g_signal_connect (this->output_node, "notify::node-state", (GCallback) on_node_state_notify, this);

  g_signal_connect (this, "notify", (GCallback) on_property_notify, this);

  G_OBJECT_CLASS (pinos_link_parent_class)->constructed (object);

  on_property_notify (G_OBJECT (this), NULL, this);
  g_debug ("link %p: constructed %p:%d:%d -> %p:%d:%d", this,
                                                  this->output_node, this->output_id, this->output_port,
                                                  this->input_node, this->input_id, this->input_port);
  link_register_object (this);
}

static void
pinos_link_dispose (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);

  g_debug ("link %p: dispose", this);

  if (this->input_node)
    g_signal_handlers_disconnect_by_data (this->input_node, this);
  if (this->output_node)
    g_signal_handlers_disconnect_by_data (this->output_node, this);

  g_signal_emit (this, signals[SIGNAL_REMOVE], 0, NULL);

  g_clear_object (&this->input_node);
  g_clear_object (&this->output_node);

  link_unregister_object (this);

  G_OBJECT_CLASS (pinos_link_parent_class)->dispose (object);
}

static void
pinos_link_finalize (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);
  PinosLinkPrivate *priv = this->priv;

  g_debug ("link %p: finalize", this);
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
                                   PROP_OUTPUT_NODE,
                                   g_param_spec_object ("output-node",
                                                        "Output Node",
                                                        "The output node",
                                                        PINOS_TYPE_NODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OUTPUT_ID,
                                   g_param_spec_uint ("output-id",
                                                      "Output Id",
                                                      "The output id",
                                                      0,
                                                      G_MAXUINT,
                                                      -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OUTPUT_PORT,
                                   g_param_spec_uint ("output-port",
                                                      "Output Port",
                                                      "The output port",
                                                      0,
                                                      G_MAXUINT,
                                                      -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_INPUT_NODE,
                                   g_param_spec_object ("input-node",
                                                        "Input Node",
                                                        "The input node",
                                                        PINOS_TYPE_NODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_INPUT_ID,
                                   g_param_spec_uint ("input-id",
                                                      "Input Id",
                                                      "The input id",
                                                      0,
                                                      G_MAXUINT,
                                                      -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_INPUT_PORT,
                                   g_param_spec_uint ("input-port",
                                                      "Input Port",
                                                      "The input port",
                                                      0,
                                                      G_MAXUINT,
                                                      -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FORMAT_FILTER,
                                   g_param_spec_boxed ("format-filter",
                                                       "format Filter",
                                                       "The format filter",
                                                       G_TYPE_PTR_ARRAY,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the link",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state of the link",
                                                      PINOS_TYPE_LINK_STATE,
                                                      PINOS_LINK_STATE_INIT,
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
}

static void
pinos_link_init (PinosLink * this)
{
  PinosLinkPrivate *priv = this->priv = PINOS_LINK_GET_PRIVATE (this);

  priv->iface = pinos_link1_skeleton_new ();
  g_debug ("link %p: new", this);
  priv->state = PINOS_LINK_STATE_INIT;
}

/**
 * pinos_link_remove:
 * @link: a #PinosLink
 *
 * Trigger removal of @link
 */
void
pinos_link_remove (PinosLink *this)
{
  g_debug ("link %p: remove", this);
  g_signal_emit (this, signals[SIGNAL_REMOVE], 0, NULL);
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
pinos_link_get_object_path (PinosLink *this)
{
  PinosLinkPrivate *priv;

  g_return_val_if_fail (PINOS_IS_LINK (this), NULL);
  priv = this->priv;

  return priv->object_path;
}

PinosLinkState
pinos_link_get_state (PinosLink  *this,
                      GError    **error)
{
  PinosLinkPrivate *priv;

  g_return_val_if_fail (PINOS_IS_LINK (this), PINOS_LINK_STATE_ERROR);
  priv = this->priv;

  if (error)
    *error = priv->error;

  return priv->state;
}

gboolean
pinos_link_activate (PinosLink *this)
{
  g_return_val_if_fail (PINOS_IS_LINK (this), FALSE);

  check_states (this);

  return TRUE;
}

gboolean
pinos_link_deactivate (PinosLink *this)
{
  return TRUE;
}
