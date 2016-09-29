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

#define MAX_BUFFERS     16

struct _PinosLinkPrivate
{
  PinosDaemon *daemon;
  PinosLink1 *iface;

  gchar *object_path;
  GPtrArray *format_filter;
  PinosProperties *properties;

  PinosLinkState state;
  GError *error;

  uint32_t async_busy;

  gboolean allocated;
  SpaBuffer *buffers[MAX_BUFFERS];
  unsigned int n_buffers;
};

G_DEFINE_TYPE (PinosLink, pinos_link, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_OUTPUT_PORT,
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

    case PROP_OUTPUT_PORT:
      g_value_set_pointer (value, this->output);
      break;

    case PROP_INPUT_PORT:
      g_value_set_pointer (value, this->input);
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

    case PROP_OUTPUT_PORT:
      this->output = g_value_get_pointer (value);
      break;

    case PROP_INPUT_PORT:
      this->input = g_value_get_pointer (value);
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
    g_debug ("link %p: update state %s -> %s", link,
        pinos_link_state_as_string (priv->state),
        pinos_link_state_as_string (state));
    priv->state = state;
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
    if ((res = spa_node_port_enum_formats (this->input->node->node,
                                           this->input->port,
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
    g_debug ("Try filter:");
    spa_debug_format (filter);

    if ((res = spa_node_port_enum_formats (this->output->node->node,
                                           this->output->port,
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
    g_debug ("Got filtered:");
    spa_debug_format (format);
    spa_format_fixate (format);
  } else if (in_state == SPA_NODE_STATE_CONFIGURE && out_state > SPA_NODE_STATE_CONFIGURE) {
    /* only input needs format */
    if ((res = spa_node_port_get_format (this->output->node->node,
                                         this->output->port,
                                         (const SpaFormat **)&format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error get output format: %d", res);
      goto error;
    }
  } else if (out_state == SPA_NODE_STATE_CONFIGURE && in_state > SPA_NODE_STATE_CONFIGURE) {
    /* only output needs format */
    if ((res = spa_node_port_get_format (this->input->node->node,
                                         this->input->port,
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
    if ((res = spa_node_port_set_format (this->output->node->node,
                                         this->output->port,
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
    if ((res = spa_node_port_set_format (this->input->node->node,
                                         this->input->port,
                                         SPA_PORT_FORMAT_FLAG_NEAREST,
                                         format)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_FORMAT_NEGOTIATION,
                   "error set input format: %d", res);
      goto error;
    }
  }
  return res;

error:
  {
    pinos_link_report_error (this, error);
    return res;
  }
}

static void *
find_param (const SpaPortInfo *info, SpaAllocParamType type)
{
  unsigned int i;

  for (i = 0; i < info->n_params; i++) {
    if (info->params[i]->type == type)
      return info->params[i];
  }
  return NULL;
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

  g_debug ("link %p: doing alloc buffers %p %p", this, this->output->node, this->input->node);
  /* find out what's possible */
  if ((res = spa_node_port_get_info (this->output->node->node, this->output->port, &oinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get output port info: %d", res);
    goto error;
  }
  if ((res = spa_node_port_get_info (this->input->node->node, this->input->port, &iinfo)) < 0) {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "error get input port info: %d", res);
    goto error;
  }
  in_flags = iinfo->flags;
  out_flags = oinfo->flags;

  if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
    this->output->node->live = true;
    this->input->node->live = true;
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
      out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
      in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
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

  if (!priv->allocated) {
    SpaAllocParamBuffers *in_alloc, *out_alloc;
    guint max_buffers = MAX_BUFFERS;
    SpaBufferAllocFlags flags = 0;

    max_buffers = MAX_BUFFERS;
    in_alloc = find_param (iinfo, SPA_ALLOC_PARAM_TYPE_BUFFERS);
    if (in_alloc)
      max_buffers = in_alloc->max_buffers == 0 ? max_buffers : SPA_MIN (in_alloc->max_buffers, max_buffers);
    out_alloc = find_param (oinfo, SPA_ALLOC_PARAM_TYPE_BUFFERS);
    if (out_alloc)
      max_buffers = out_alloc->max_buffers == 0 ? max_buffers : SPA_MIN (out_alloc->max_buffers, max_buffers);

    if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
        (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS))
      flags |= SPA_BUFFER_ALLOC_FLAG_NO_MEM;

    priv->n_buffers = max_buffers;
    if ((res = spa_buffer_alloc (flags,
                                 oinfo->params, oinfo->n_params,
                                 priv->buffers,
                                 &priv->n_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error buffer alloc: %d", res);
      goto error;
    }
    g_debug ("allocated %d buffers %p", priv->n_buffers, priv->buffers);
    priv->allocated = TRUE;

    if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->input->node->node, this->input->port,
                                              oinfo->params, oinfo->n_params,
                                              priv->buffers, &priv->n_buffers)) < 0) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_BUFFER_ALLOCATION,
                     "error alloc input buffers: %d", res);
        goto error;
      }
      this->input->allocated = TRUE;
      g_debug ("allocated %d buffers %p from input port", priv->n_buffers, priv->buffers);
    }
    else if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
      if ((res = spa_node_port_alloc_buffers (this->output->node->node, this->output->port,
                                              iinfo->params, iinfo->n_params,
                                              priv->buffers, &priv->n_buffers)) < 0) {
        g_set_error (&error,
                     PINOS_ERROR,
                     PINOS_ERROR_BUFFER_ALLOCATION,
                     "error alloc output buffers: %d", res);
        goto error;
      }
      this->output->allocated = TRUE;
      g_debug ("allocated %d buffers %p from output port", priv->n_buffers, priv->buffers);
    }
  }
  if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    g_debug ("using %d buffers %p on input port", priv->n_buffers, priv->buffers);
    if ((res = spa_node_port_use_buffers (this->input->node->node, this->input->port,
                                          priv->buffers, priv->n_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use input buffers: %d", res);
      goto error;
    }
    this->input->allocated = FALSE;
  }
  else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
    g_debug ("using %d buffers %p on output port", priv->n_buffers, priv->buffers);
    if ((res = spa_node_port_use_buffers (this->output->node->node, this->output->port,
                                          priv->buffers, priv->n_buffers)) < 0) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_BUFFER_ALLOCATION,
                   "error use output buffers: %d", res);
      goto error;
    }
    this->output->allocated = FALSE;
  } else {
    g_set_error (&error,
                 PINOS_ERROR,
                 PINOS_ERROR_BUFFER_ALLOCATION,
                 "no common buffer alloc found");
      goto error;
    this->input->allocated = FALSE;
    this->output->allocated = FALSE;
  }

  return res;

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
  else if (in_state == SPA_NODE_STATE_STREAMING && out_state == SPA_NODE_STATE_STREAMING) {
    pinos_link_update_state (this, PINOS_LINK_STATE_RUNNING);
  } else {
    pinos_link_update_state (this, PINOS_LINK_STATE_PAUSED);

    if (in_state == SPA_NODE_STATE_PAUSED) {
      pinos_node_set_state (this->input->node, PINOS_NODE_STATE_RUNNING);
      if (pinos_node_get_state (this->input->node) != PINOS_NODE_STATE_RUNNING)
        res = SPA_RESULT_RETURN_ASYNC (0);
    }

    if (out_state == SPA_NODE_STATE_PAUSED) {
      pinos_node_set_state (this->output->node, PINOS_NODE_STATE_RUNNING);
      if (pinos_node_get_state (this->output->node) != PINOS_NODE_STATE_RUNNING)
        res = SPA_RESULT_RETURN_ASYNC (0);
    }
  }
  return res;
}

static SpaResult
check_states (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  SpaResult res;
  SpaNodeState in_state, out_state;

again:
   if (priv->async_busy != SPA_ID_INVALID)
     return SPA_RESULT_OK;

  in_state = this->input->node->node->state;
  out_state = this->output->node->node->state;

  g_debug ("link %p: input state %d, output state %d", this, in_state, out_state);

  if ((res = do_negotiate (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_allocation (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if ((res = do_start (this, in_state, out_state)) != SPA_RESULT_OK)
    goto exit;

  if (this->input->node->node->state != in_state)
    goto again;
  if (this->output->node->node->state != out_state)
    goto again;

  return SPA_RESULT_OK;

exit:
  if (SPA_RESULT_IS_ASYNC (res)) {
    priv->async_busy = SPA_RESULT_ASYNC_SEQ (res);
    g_debug ("link %p: waiting for async complete %d", this, priv->async_busy);
  }
  return res;
}

static gboolean
do_check_states (PinosLink *this)
{
  PinosLinkPrivate *priv = this->priv;
  priv->async_busy = SPA_ID_INVALID;
  check_states (this);
  return G_SOURCE_REMOVE;
}

static void
on_async_complete_notify (PinosNode  *node,
                          guint       seq,
                          guint       res,
                          PinosLink  *this)
{
  g_debug ("link %p: node %p async complete %d %d", this, node, seq, res);
  g_idle_add ((GSourceFunc) do_check_states, this);
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosLink *this = user_data;
  PinosLinkPrivate *priv = this->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "output-port") == 0) {
    pinos_link1_set_output_node (priv->iface, pinos_node_get_object_path (this->output->node));
    pinos_link1_set_output_port (priv->iface, this->output->port);
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "input-port") == 0) {
    pinos_link1_set_input_node (priv->iface, pinos_node_get_object_path (this->input->node));
    pinos_link1_set_input_port (priv->iface, this->input->port);
  }
}

static void
on_node_remove (PinosNode *node, PinosLink *this)
{
  g_signal_handlers_disconnect_by_data (node, this);
  if (node == this->input->node)
    this->input = NULL;
  else
    this->output = NULL;

  pinos_link_update_state (this, PINOS_LINK_STATE_UNLINKED);
}

static void
pinos_link_constructed (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);

  g_signal_connect (this->input->node, "remove", (GCallback) on_node_remove, this);
  g_signal_connect (this->output->node, "remove", (GCallback) on_node_remove, this);

  g_signal_connect (this->input->node, "async-complete", (GCallback) on_async_complete_notify, this);
  g_signal_connect (this->output->node, "async-complete", (GCallback) on_async_complete_notify, this);

  g_signal_connect (this, "notify", (GCallback) on_property_notify, this);

  G_OBJECT_CLASS (pinos_link_parent_class)->constructed (object);

  on_property_notify (G_OBJECT (this), NULL, this);
  g_debug ("link %p: constructed %p:%d -> %p:%d", this,
                                                  this->output->node, this->output->port,
                                                  this->input->node, this->input->port);
  link_register_object (this);
}

static void
pinos_link_dispose (GObject * object)
{
  PinosLink *this = PINOS_LINK (object);

  g_debug ("link %p: dispose", this);

  if (this->input->node)
    g_signal_handlers_disconnect_by_data (this->input->node, this);
  if (this->output->node)
    g_signal_handlers_disconnect_by_data (this->output->node, this);

  g_signal_emit (this, signals[SIGNAL_REMOVE], 0, NULL);

  this->input = NULL;
  this->output = NULL;

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
                                   PROP_OUTPUT_PORT,
                                   g_param_spec_pointer ("output-port",
                                                         "Output Port",
                                                         "The output port",
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_INPUT_PORT,
                                   g_param_spec_pointer ("input-port",
                                                         "Input Port",
                                                         "The input port",
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

  spa_ringbuffer_init (&this->ringbuffer, SPA_N_ELEMENTS (this->queue));
  check_states (this);

  return TRUE;
}

gboolean
pinos_link_deactivate (PinosLink *this)
{
  spa_ringbuffer_clear (&this->ringbuffer);
  return TRUE;
}
