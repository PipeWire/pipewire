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

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <gio/gio.h>

#include <spa/include/spa/node.h>
#include <spa/include/spa/video/format.h>

#include "spa-v4l2-source.h"

#define PINOS_SPA_V4L2_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2SourcePrivate))

struct _PinosSpaV4l2SourcePrivate
{
  SpaPollFd fds[16];
  unsigned int n_fds;
  SpaPollItem poll;

  gboolean running;
  pthread_t thread;

  GBytes *format;
};

enum {
  PROP_0,
};

G_DEFINE_TYPE (PinosSpaV4l2Source, pinos_spa_v4l2_source, PINOS_TYPE_NODE);

static SpaResult
make_node (SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
  SpaResult res;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  while (true) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    handle = calloc (1, factory->size);
    if ((res = factory->init (factory, handle)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = handle->get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

static void *
loop (void *user_data)
{
  PinosSpaV4l2Source *this = user_data;
  PinosSpaV4l2SourcePrivate *priv = this->priv;
  int r;

  g_debug ("spa-v4l2-source %p: enter thread", this);
  while (priv->running) {
    SpaPollNotifyData ndata;

    r = poll ((struct pollfd *) priv->fds, priv->n_fds, -1);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0) {
      g_debug ("spa-v4l2-source %p: select timeout", this);
      break;
    }
    if (priv->poll.after_cb) {
      ndata.fds = priv->poll.fds;
      ndata.n_fds = priv->poll.n_fds;
      ndata.user_data = priv->poll.user_data;
      priv->poll.after_cb (&ndata);
    }
  }
  g_debug ("spa-v4l2-source %p: leave thread", this);
  return NULL;
}

static void
on_source_event (SpaNode *node, SpaEvent *event, void *user_data)
{
  PinosSpaV4l2Source *this = user_data;
  PinosSpaV4l2SourcePrivate *priv = this->priv;

  switch (event->type) {
    case SPA_EVENT_TYPE_HAVE_OUTPUT:
    {
      SpaOutputInfo info[1] = { 0, };
      SpaResult res;
      GList *walk;

      if ((res = spa_node_port_pull_output (node, 1, info)) < 0)
        g_debug ("spa-v4l2-source %p: got pull error %d, %d", this, res, info[0].status);

      walk = pinos_node_get_ports (PINOS_NODE (this));
      for (; walk; walk = g_list_next (walk)) {
        PinosPort *port = walk->data;
        GError *error = NULL;

        if (!pinos_port_send_buffer (port, info[0].buffer_id, &error)) {
          g_debug ("send failed: %s", error->message);
          g_clear_error (&error);
        }
      }
      break;
    }

    case SPA_EVENT_TYPE_ADD_POLL:
    {
      SpaPollItem *poll = event->data;
      int err;

      priv->poll = *poll;
      priv->fds[0] = poll->fds[0];
      priv->n_fds = 1;
      priv->poll.fds = priv->fds;

      if (!priv->running) {
        priv->running = true;
        if ((err = pthread_create (&priv->thread, NULL, loop, this)) != 0) {
          g_debug ("spa-v4l2-source %p: can't create thread: %s", this, strerror (err));
          priv->running = false;
        }
      }
      break;
    }
    case SPA_EVENT_TYPE_REMOVE_POLL:
    {
      if (priv->running) {
        priv->running = false;
        pthread_join (priv->thread, NULL);
      }
      break;
    }
    case SPA_EVENT_TYPE_STATE_CHANGE:
    {
      SpaEventStateChange *sc = event->data;

      pinos_node_update_node_state (PINOS_NODE (this), sc->state);
      break;
    }
    default:
      g_debug ("got event %d", event->type);
      break;
  }
}

static void
setup_node (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaProps *props;
  SpaPropValue value;

  spa_node_set_event_callback (node->node, on_source_event, this);

  if ((res = spa_node_get_props (node->node, &props)) < 0)
    g_debug ("got get_props error %d", res);

  value.type = SPA_PROP_TYPE_STRING;
  value.value = "/dev/video1";
  value.size = strlen (value.value)+1;
  spa_props_set_prop (props, spa_props_index_for_name (props, "device"), &value);

  if ((res = spa_node_set_props (node->node, props)) < 0)
    g_debug ("got set_props error %d", res);
}

static void
pause_pipeline (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;
  SpaCommand cmd;

  g_debug ("spa-v4l2-source %p: pause pipeline", this);

  cmd.type = SPA_COMMAND_PAUSE;
  if ((res = spa_node_send_command (node->node, &cmd)) < 0)
    g_debug ("got error %d", res);
}

static void
suspend_pipeline (PinosSpaV4l2Source *this)
{
  PinosNode *node = PINOS_NODE (this);
  SpaResult res;

  g_debug ("spa-v4l2-source %p: suspend pipeline", this);

  if ((res = spa_node_port_set_format (node->node, 0, 0, NULL)) < 0) {
    g_warning ("error unset format output: %d", res);
  }
}

static void
destroy_pipeline (PinosSpaV4l2Source *this)
{
  g_debug ("spa-v4l2-source %p: destroy pipeline", this);
}

static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosSpaV4l2Source *this = PINOS_SPA_V4L2_SOURCE (node);

  g_debug ("spa-source %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      suspend_pipeline (this);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      break;

    case PINOS_NODE_STATE_IDLE:
      pause_pipeline (this);
      break;

    case PINOS_NODE_STATE_RUNNING:
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (node, state);
  return TRUE;
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
on_activate (PinosPort *port, gpointer user_data)
{
  PinosSpaV4l2Source *source = user_data;

  pinos_node_report_busy (PINOS_NODE (source));

  return TRUE;
}

static void
on_deactivate (PinosPort *port, gpointer user_data)
{
  PinosSpaV4l2Source *source = user_data;

  pinos_node_report_idle (PINOS_NODE (source));
}

static gboolean
remove_port (PinosNode       *node,
             PinosPort       *port)
{
  return PINOS_NODE_CLASS (pinos_spa_v4l2_source_parent_class)
                    ->remove_port (node, port);
}

static void
source_constructed (GObject * object)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->constructed (object);

  setup_node (source);
}

static void
source_finalize (GObject * object)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (object);

  g_debug ("spa-source %p: dispose", source);
  destroy_pipeline (source);

  G_OBJECT_CLASS (pinos_spa_v4l2_source_parent_class)->finalize (object);
}

static gboolean
on_received_buffer (PinosPort *port, uint32_t buffer_id, GError **error, gpointer user_data)
{
  return FALSE;
}

static gboolean
on_received_event (PinosPort *port, SpaEvent *event, GError **error, gpointer user_data)
{
  PinosNode *node = user_data;
  SpaResult res;

  switch (event->type) {
    case SPA_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaEventReuseBuffer *rb = event->data;

      if ((res = spa_node_port_reuse_buffer (node->node,
                                             rb->port_id,
                                             rb->buffer_id)) < 0)
        g_warning ("client-node %p: error reuse buffer: %d", node, res);
      break;
    }
    default:
      if ((res = spa_node_port_push_event (node->node, port->id, event)) < 0)
        g_warning ("client-node %p: error pushing event: %d", node, res);
      break;
  }
  return TRUE;
}


static PinosPort *
add_port (PinosNode       *node,
          guint            id,
          GError         **error)
{
  PinosSpaV4l2Source *source = PINOS_SPA_V4L2_SOURCE (node);
  PinosPort *port;

  port = PINOS_NODE_CLASS (pinos_spa_v4l2_source_parent_class)
                    ->add_port (node, id, error);

  pinos_port_set_received_cb (port, on_received_buffer, on_received_event, node, NULL);

  g_debug ("connecting signals");
  g_signal_connect (port, "activate", (GCallback) on_activate, source);
  g_signal_connect (port, "deactivate", (GCallback) on_deactivate, source);

  return port;
}

static void
pinos_spa_v4l2_source_class_init (PinosSpaV4l2SourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSpaV4l2SourcePrivate));

  gobject_class->constructed = source_constructed;
  gobject_class->finalize = source_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;

  node_class->set_state = set_state;
  node_class->add_port = add_port;
  node_class->remove_port = remove_port;
}

static void
pinos_spa_v4l2_source_init (PinosSpaV4l2Source * source)
{
  source->priv = PINOS_SPA_V4L2_SOURCE_GET_PRIVATE (source);
}

PinosNode *
pinos_spa_v4l2_source_new (PinosDaemon *daemon,
                           const gchar *name,
                           PinosProperties *properties)
{
  PinosNode *node;
  SpaNode *n;
  SpaResult res;

  if ((res = make_node (&n,
                        "spa/build/plugins/v4l2/libspa-v4l2.so",
                        "v4l2-source")) < 0) {
    g_error ("can't create v4l2-source: %d", res);
    return NULL;
  }

  node = g_object_new (PINOS_TYPE_SPA_V4L2_SOURCE,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);

  return node;
}
