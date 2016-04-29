/* GStreamer
 * Copyright (C) <2016> Wim Taymans <wim.taymans@gmail.com>
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

/**
 * SECTION:element-pinossocketsink
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>

#include <gst/allocators/gstfdmemory.h>
#include <gst/net/gstnetcontrolmessagemeta.h>
#include <gst/video/video.h>

#include "gstpinossocketsink.h"
#include "gsttmpfileallocator.h"

typedef struct _MyReader MyReader;
typedef struct _MySource MySource;

struct _MyReader {
  GstBurstCacheReader reader;
  GSocket *socket;
  MySource *source;
  guint id;
};

struct _MySource {
  GSource source;
  GIOCondition condition;
  gpointer tag;
  MyReader *reader;
};

typedef gboolean (*MyReaderSourceFunc) (MyReader *reader, GIOCondition condition, gpointer user_data);

static gboolean
mysource_dispatch (GSource     *source,
                   GSourceFunc  callback,
                   gpointer     user_data)
{
  MyReaderSourceFunc func = (MyReaderSourceFunc)callback;
  MySource *mysource = (MySource *)source;
  MyReader *myreader = mysource->reader;
  guint events;
  gboolean ret;

  events = g_source_query_unix_fd (source, mysource->tag);

  ret = (*func) (myreader, events, user_data);

  return ret;
}

static GSourceFuncs mysource_funcs =
{
  NULL, NULL, /* check, prepare */
  mysource_dispatch,
  NULL, /* finalize */
  NULL,
  NULL,
};

static GQuark fdids_quark;
static GQuark orig_buffer_quark;

GST_DEBUG_CATEGORY_STATIC (pinos_socket_sink_debug);
#define GST_CAT_DEFAULT pinos_socket_sink_debug

static GstStaticPadTemplate gst_pinos_socket_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

enum
{
  PROP_0,

  PROP_NUM_HANDLES,
};

/* PinosSocketSink signals and args */
enum
{
  /* methods */
  SIGNAL_ADD,
  SIGNAL_REMOVE,

  LAST_SIGNAL
};

static guint gst_pinos_socket_sink_signals[LAST_SIGNAL] = { 0 };

#define gst_pinos_socket_sink_parent_class parent_class
G_DEFINE_TYPE (GstPinosSocketSink, gst_pinos_socket_sink, GST_TYPE_BASE_SINK);

static gboolean
gst_pinos_socket_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK (bsink);

  gst_query_add_allocation_param (query, this->allocator, NULL);
  gst_query_add_allocation_meta (query, GST_NET_CONTROL_MESSAGE_META_API_TYPE,
            NULL);

  return TRUE;
}

static void
gst_pinos_socket_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_socket_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK (object);

  switch (prop_id) {
    case PROP_NUM_HANDLES:
      g_value_set_uint (value, g_hash_table_size (this->hash));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_pinos_socket_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK (bsink);
  GstStructure *str;

  str = gst_caps_get_structure (caps, 0);
  this->pinos_input = gst_structure_has_name (str, "application/x-pinos");

  return GST_BASE_SINK_CLASS (parent_class)->set_caps (bsink, caps);
}

static void
release_fds (GstPinosSocketSink *this, GstBuffer *buffer)
{
  GArray *fdids;
  guint i;
  PinosBufferBuilder b;
  PinosPacketReleaseFDPayload r;
  PinosBuffer pbuf;
  gsize size;
  gpointer data;
  GstBuffer *outbuf;
  GstEvent *ev;

  fdids = gst_mini_object_steal_qdata (GST_MINI_OBJECT_CAST (buffer),
      fdids_quark);
  if (fdids == NULL)
    return;

  pinos_buffer_builder_init (&b);

  for (i = 0; i < fdids->len; i++) {
    r.id = g_array_index (fdids, guint32, i);
    GST_LOG ("release fd index %d", r.id);
    pinos_buffer_builder_add_release_fd_payload (&b, &r);
  }
  pinos_buffer_builder_end (&b, &pbuf);
  g_array_unref (fdids);

  data = pinos_buffer_steal (&pbuf, &size, NULL);

  outbuf = gst_buffer_new_wrapped (data, size);
  ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
          gst_structure_new ("GstNetworkMessage",
              "object", G_TYPE_OBJECT, this,
              "buffer", GST_TYPE_BUFFER, outbuf, NULL));
  gst_buffer_unref (outbuf);

  gst_pad_push_event (GST_BASE_SINK_PAD (this), ev);
  g_object_unref (this);
}

static GstFlowReturn
gst_pinos_socket_sink_render_pinos (GstPinosSocketSink * this, GstBuffer * buffer)
{
  GstMapInfo info;
  PinosBuffer pbuf;
  PinosBufferIter it;
  GArray *fdids = NULL;

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, NULL);
  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        PinosPacketFDPayload p;

        if (!pinos_buffer_iter_parse_fd_payload (&it, &p))
          continue;

        if (fdids == NULL)
          fdids = g_array_new (FALSE, FALSE, sizeof (guint32));

        GST_LOG ("track fd index %d", p.id);
        g_array_append_val (fdids, p.id);
        break;
      }
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange p;
        GstCaps * caps;

        if (!pinos_buffer_iter_parse_format_change (&it, &p))
          continue;

        caps = gst_caps_from_string (p.format);

        gst_element_post_message (GST_ELEMENT (this),
            gst_message_new_element (GST_OBJECT (this),
                gst_structure_new ("PinosPayloaderFormatChange",
                    "format", GST_TYPE_CAPS, caps, NULL)));
        gst_caps_unref (caps);
        break;
      }
      default:
        break;
    }
  }
  gst_buffer_unmap (buffer, &info);
  pinos_buffer_clear (&pbuf);

  if (fdids != NULL) {
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buffer),
        fdids_quark, fdids, NULL);
    gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (buffer),
        (GstMiniObjectNotify) release_fds, g_object_ref (this));
  }
  gst_burst_cache_queue_buffer (this->cache, gst_buffer_ref (buffer));

  return GST_FLOW_OK;
}

static GstMemory *
gst_pinos_socket_sink_get_fd_memory (GstPinosSocketSink * this, GstBuffer * buffer, gboolean *tmpfile)
{
  GstMemory *mem = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
    *tmpfile = gst_is_tmpfile_memory (mem);
  } else {
    GstMapInfo info;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};
    gsize size = gst_buffer_get_size (buffer);
    GST_INFO_OBJECT (this, "Buffer cannot be sent without copying");
    mem = gst_allocator_alloc (this->allocator, size, &params);
    if (!gst_memory_map (mem, &info, GST_MAP_WRITE))
      return NULL;
    gst_buffer_extract (buffer, 0, info.data, size);
    gst_memory_unmap (mem, &info);
    *tmpfile = TRUE;
  }
  return mem;
}

static GstFlowReturn
gst_pinos_socket_sink_render_other (GstPinosSocketSink * this, GstBuffer * buffer)
{
  GstMemory *fdmem = NULL;
  GError *err = NULL;
  GstBuffer *outbuf;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  PinosPacketHeader hdr;
  PinosPacketFDPayload p;
  gsize size;
  gpointer data;
  GSocketControlMessage *msg;
  gboolean tmpfile = TRUE;

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (buffer);
  hdr.pts = GST_BUFFER_PTS (buffer) + GST_ELEMENT_CAST (this)->base_time;
  hdr.dts_offset = 0;

  pinos_buffer_builder_init (&builder);
  pinos_buffer_builder_add_header (&builder, &hdr);

  fdmem = gst_pinos_socket_sink_get_fd_memory (this, buffer, &tmpfile);
  p.fd_index = pinos_buffer_builder_add_fd (&builder, gst_fd_memory_get_fd (fdmem), &err);
  if (p.fd_index == -1)
    goto add_fd_failed;
  p.id = pinos_fd_manager_get_id (this->fdmanager);
  p.offset = fdmem->offset;
  p.size = fdmem->size;
  pinos_buffer_builder_add_fd_payload (&builder, &p);

  GST_LOG ("send %d %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT,
      p.id, hdr.pts, GST_BUFFER_PTS (buffer), GST_ELEMENT_CAST (this)->base_time);

  pinos_buffer_builder_end (&builder, &pbuf);
  gst_memory_unref(fdmem);
  fdmem = NULL;

  data = pinos_buffer_steal (&pbuf, &size, &msg);

  outbuf = gst_buffer_new_wrapped (data, size);
  GST_BUFFER_PTS (outbuf) = GST_BUFFER_PTS (buffer);
  GST_BUFFER_DTS (outbuf) = GST_BUFFER_DTS (buffer);
  GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (buffer);
  GST_BUFFER_OFFSET (outbuf) = GST_BUFFER_OFFSET (buffer);
  GST_BUFFER_OFFSET_END (outbuf) = GST_BUFFER_OFFSET_END (buffer);

  if (!tmpfile) {
    GArray *fdids;
    /* we are using the original buffer fd in the control message, we need
     * to make sure it is not reused before everyone is finished with it.
     * We tag the output buffer with the array of fds in it and the original
     * buffer (to keep it alive). All clients that receive the fd will
     * increment outbuf refcount, all clients that do release-fd on the fd
     * will decrease the refcount again. */
    fdids = g_array_new (FALSE, FALSE, sizeof (guint32));
    g_array_append_val (fdids, p.id);
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (outbuf),
        fdids_quark, fdids, (GDestroyNotify) g_array_unref);
    gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (outbuf),
        orig_buffer_quark, gst_buffer_ref (buffer), (GDestroyNotify) gst_buffer_unref);
  }
  gst_buffer_add_net_control_message_meta (outbuf, msg);
  g_object_unref (msg);

  gst_burst_cache_queue_buffer (this->cache, outbuf);

  return GST_FLOW_OK;

  /* ERRORS */
add_fd_failed:
  {
    GST_WARNING_OBJECT (this, "Adding fd failed: %s", err->message);
    gst_memory_unref(fdmem);
    g_clear_error (&err);

    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_pinos_socket_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK (bsink);

  if (this->pinos_input)
    return gst_pinos_socket_sink_render_pinos (this, buffer);
  else
    return gst_pinos_socket_sink_render_other (this, buffer);
}

static gboolean
gst_pinos_socket_sink_start (GstBaseSink * basesink)
{
  return TRUE;
}

static gboolean
gst_pinos_socket_sink_stop (GstBaseSink * basesink)
{
  return TRUE;
}

static gpointer
socketsink_loop (GstPinosSocketSink * this)
{
  g_main_loop_run (this->loop);
  return NULL;
}

static gboolean
gst_pinos_socket_sink_open (GstPinosSocketSink * this)
{
  GError *error = NULL;

  this->context = g_main_context_new ();
  this->loop = g_main_loop_new (this->context, TRUE);
  GST_DEBUG ("context %p, loop %p", this->context, this->loop);

  this->thread = g_thread_try_new ("PinosSocketSink",
                                   (GThreadFunc) socketsink_loop,
                                   this,
                                   &error);
  if (this->thread == NULL)
    goto thread_error;

  return TRUE;

  /* ERRORS */
thread_error:
  {
    GST_ELEMENT_ERROR (this, RESOURCE, FAILED,
        ("Failed to start mainloop thread: %s", error->message), (NULL));
    g_clear_error (&error);
    g_clear_pointer (&this->loop, g_main_loop_unref);
    g_clear_pointer (&this->context, g_main_context_unref);
    return FALSE;
  }
}

static gboolean
gst_pinos_socket_sink_close (GstPinosSocketSink * this)
{
  gst_burst_cache_remove_buffers (this->cache);

  GST_DEBUG ("context %p, loop %p", this->context, this->loop);
  g_main_loop_quit (this->loop);
  g_thread_join (this->thread);
  this->thread = NULL;
  g_clear_pointer (&this->loop, g_main_loop_unref);
  g_clear_pointer (&this->context, g_main_context_unref);
  g_hash_table_remove_all (this->hash);

  return TRUE;
}

static GstStateChangeReturn
gst_pinos_socket_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pinos_socket_sink_open (this))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pinos_socket_sink_close (this);
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
open_failed:
  {
    return GST_STATE_CHANGE_FAILURE;
  }
}

static void
myreader_receive_buffer (GstPinosSocketSink *this, MyReader *myreader)
{
  MySource *mysource = myreader->source;
  gssize navail, nread, maxmem;
  GstEvent *ev;
  gchar *mem;
  PinosBuffer pbuf;
  PinosBufferIter it;
  PinosBufferBuilder b;
  const gchar *client_path;
  gboolean have_out = FALSE;

  navail = g_socket_get_available_bytes (myreader->socket);
  maxmem = MAX (navail, 1);
  mem = g_malloc (maxmem);
  nread = g_socket_receive (myreader->socket, mem, maxmem, NULL, NULL);

  if (nread <= 0) {
    GST_DEBUG ("client closed");
    mysource->condition &= ~G_IO_IN;
    g_source_modify_unix_fd ((GSource *)mysource, mysource->tag, mysource->condition);
    g_free (mem);
    return;
  }

  client_path = g_object_get_data (G_OBJECT (myreader->socket), "pinos-client-path");
  if (client_path == NULL)
    return;

  if (this->pinos_input) {
    pinos_buffer_builder_init (&b);
  }

  pinos_buffer_init_data (&pbuf, mem, maxmem, NULL);
  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD:
      {
        PinosPacketReleaseFDPayload p;
        gint id;

        if (!pinos_buffer_iter_parse_release_fd_payload (&it, &p))
          continue;

        id = p.id;

        GST_LOG ("fd index %d for client %s is released", id, client_path);
        pinos_fd_manager_remove (this->fdmanager, client_path, id);
        break;
      }
     case PINOS_PACKET_TYPE_REFRESH_REQUEST:
      {
        PinosPacketRefreshRequest p;

        if (!pinos_buffer_iter_parse_refresh_request (&it, &p))
          continue;

        GST_LOG ("refresh request");
        if (!this->pinos_input) {
          gst_pad_push_event (GST_BASE_SINK_PAD (this),
              gst_video_event_new_upstream_force_key_unit (p.pts,
              p.request_type == 1, 0));
        } else {
          pinos_buffer_builder_add_refresh_request (&b, &p);
          have_out = TRUE;
        }
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_clear (&pbuf);
  g_free (mem);

  if (this->pinos_input) {
    GstBuffer *outbuf;
    gsize size;
    gpointer data;

    if (have_out) {
      pinos_buffer_builder_end (&b, &pbuf);

      data = pinos_buffer_steal (&pbuf, &size, NULL);

      outbuf = gst_buffer_new_wrapped (data, size);
      ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
              gst_structure_new ("GstNetworkMessage",
                  "object", G_TYPE_OBJECT, this,
                  "buffer", GST_TYPE_BUFFER, outbuf, NULL));
      gst_buffer_unref (outbuf);

      gst_pad_push_event (GST_BASE_SINK_PAD (this), ev);
    } else {
      pinos_buffer_builder_clear (&b);
    }
  }
}

static void
myreader_callback (GstBurstCache *cache,
                   GstBurstCacheReader *reader,
                   gpointer user_data)
{
  MyReader *myreader = (MyReader *) reader;
  MySource *mysource = myreader->source;

  GST_LOG ("%p: callback", reader);
  mysource->condition |= G_IO_OUT;
  g_source_modify_unix_fd ((GSource *)mysource, mysource->tag, mysource->condition);
}

#define VEC_MAX 8
#define CMSG_MAX 255

static void
myreader_send_buffer (GstPinosSocketSink *this, MyReader *myreader, GstBuffer *buf)
{
  GstMapInfo maps[VEC_MAX];
  GOutputVector vec[VEC_MAX];
  GSocketControlMessage *cmsgs[CMSG_MAX];
  guint i, mem_len;
  gpointer iter_state = NULL;
  GstMeta *meta;
  gsize msg_count = 0;
  gssize wrote;

  mem_len = MIN (gst_buffer_n_memory (buf), VEC_MAX);

  for (i = 0; i < mem_len; i++) {
    GstMapInfo map = { 0 };
    GstMemory *mem = gst_buffer_peek_memory (buf, i);

    if (!gst_memory_map (mem, &map, GST_MAP_READ))
      g_error ("Unable to map memory %p.  This should never happen.", mem);

    vec[i].buffer = map.data;
    vec[i].size = map.size;

    maps[i] = map;
  }
  while ((meta = gst_buffer_iterate_meta (buf, &iter_state)) != NULL
      && msg_count < CMSG_MAX) {
    if (meta->info->api == GST_NET_CONTROL_MESSAGE_META_API_TYPE)
      cmsgs[msg_count++] = ((GstNetControlMessageMeta *) meta)->message;
  }

  wrote = g_socket_send_message (myreader->socket, NULL, vec, mem_len, cmsgs, msg_count, 0,
      NULL, NULL);

  for (i = 0; i < mem_len; i++)
    gst_memory_unmap (maps[i].memory, &maps[i]);

  if (wrote < 0) {
    GST_DEBUG_OBJECT (this, "error sending to reader");
  } else {
    GArray *fdids;
    const gchar *client_path;

    fdids = gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buf), fdids_quark);
    if (fdids == NULL)
      return;

    /* get the client path of this socket */
    client_path = g_object_get_data (G_OBJECT (myreader->socket), "pinos-client-path");
    if (client_path == NULL)
      return;

    for (i = 0; i < fdids->len; i++) {
      gint id = g_array_index (fdids, guint32, i);
      /* now store the id/client-path/buffer in the fdmanager */
      GST_LOG ("fd index %d, client %s increment refcount of buffer %p", id, client_path, buf);
      pinos_fd_manager_add (this->fdmanager,
                            client_path, id,
                            gst_buffer_ref (buf),
                            (GDestroyNotify) gst_buffer_unref);
    }
  }
}

static gboolean
myreader_source_func (GstBurstCacheReader *reader, GIOCondition condition, gpointer user_data)
{
  GstPinosSocketSink *this = user_data;
  MyReader *myreader = (MyReader *) reader;
  MySource *mysource = myreader->source;

  GST_LOG ("%p: io condition %d", reader, condition);

  if (condition & (G_IO_HUP | G_IO_ERR)) {
    GST_DEBUG ("client error");
    return FALSE;
  }
  if (condition & G_IO_IN) {
    myreader_receive_buffer (this, myreader);
  }
  if (condition & G_IO_OUT) {
    GstBuffer *buf = NULL;
    GstBurstCacheResult res;

    res = gst_burst_cache_get_buffer (this->cache, reader, &buf);

    switch (res) {
      case GST_BURST_CACHE_RESULT_ERROR:
        break;
      case GST_BURST_CACHE_RESULT_OK:
        break;
      case GST_BURST_CACHE_RESULT_WAIT:
        mysource->condition &= ~G_IO_OUT;
        g_source_modify_unix_fd ((GSource *)mysource, mysource->tag, mysource->condition);
        break;
      case GST_BURST_CACHE_RESULT_EOS:
        gst_burst_cache_remove_reader (this->cache, reader, FALSE);
        break;
    }
    if (buf) {
      myreader_send_buffer (this, myreader, buf);
      gst_buffer_unref (buf);
    }
  }

  return TRUE;
}

static void
myreader_destroy (MyReader *myreader)
{
  gst_burst_cache_reader_destroy ((GstBurstCacheReader *)myreader);
  g_clear_object (&myreader->socket);
  g_source_destroy ((GSource*) myreader->source);
  myreader->id = 0;
}

static void
gst_pinos_socket_sink_add (GstPinosSocketSink * this, GSocket *socket)
{
  GstBurstCacheReader *reader;
  MyReader *myreader;
  MySource *mysource;
  int fd;

  fd = g_socket_get_fd (socket);

  if (g_hash_table_lookup (this->hash, GINT_TO_POINTER (fd)))
    return;

  reader = gst_burst_cache_reader_new (this->cache,
                                       (GstBurstCacheReaderCallback) myreader_callback,
                                       this,
                                       NULL);

  reader->hook.destroy = (GDestroyNotify) myreader_destroy;
  myreader = (MyReader *)reader;
  myreader->socket = g_object_ref (socket);

  mysource = (MySource*) g_source_new (&mysource_funcs, sizeof (MySource));
  mysource->reader = myreader;
  mysource->condition = G_IO_IN;
  mysource->tag = g_source_add_unix_fd ((GSource*)mysource, fd, mysource->condition);

  myreader->source = mysource;
  g_source_set_callback ((GSource*)mysource,
                         (GSourceFunc) myreader_source_func,
                         this, NULL);
  myreader->id = g_source_attach ((GSource*)mysource, this->context);

  g_hash_table_insert (this->hash, GINT_TO_POINTER (fd), reader);

  gst_burst_cache_add_reader (this->cache, reader);
}

static void
gst_pinos_socket_sink_remove (GstPinosSocketSink * this, GSocket *socket, gboolean drain)
{
  GstBurstCacheReader *reader;
  MyReader *myreader;
  int fd;

  fd = g_socket_get_fd (socket);

  myreader = g_hash_table_lookup (this->hash, GINT_TO_POINTER (fd));
  if (myreader == NULL)
    return;

  g_hash_table_remove (this->hash, GINT_TO_POINTER (fd));

  reader = (GstBurstCacheReader *) myreader;
  gst_burst_cache_remove_reader (this->cache, reader, drain);
}

static void
gst_pinos_socket_sink_finalize (GObject * object)
{
  GstPinosSocketSink *this = GST_PINOS_SOCKET_SINK (object);

  g_clear_pointer (&this->hash, g_hash_table_unref);
  g_clear_pointer (&this->cache, g_object_unref);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_socket_sink_class_init (GstPinosSocketSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->finalize = gst_pinos_socket_sink_finalize;
  gobject_class->set_property = gst_pinos_socket_sink_set_property;
  gobject_class->get_property = gst_pinos_socket_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_NUM_HANDLES,
      g_param_spec_uint ("num-handles", "Number of handles",
          "The current number of client handles",
          0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state = gst_pinos_socket_sink_change_state;

  /**
   * GstPinosSocketSink::add:
   * @gstpinossocketsink: the pinossocketsink element to emit this signal on
   * @socket:             the socket to add to pinossocketsink
   *
   * Hand the given open file descriptor to pinossocketsink to write to.
   */
  gst_pinos_socket_sink_signals[SIGNAL_ADD] =
      g_signal_new ("add", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GstPinosSocketSinkClass, add), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, G_TYPE_SOCKET);

  /**
   * GstPinosSocketSink::remove:
   * @gstpinossocketsink: the pinossocketsink element to emit this signal on
   * @socket:             the socket to remove from pinossocketsink
   * @drain:              if pending data should be written first.
   *
   * Remove the given open file descriptor from pinossocketsink.
   */
  gst_pinos_socket_sink_signals[SIGNAL_REMOVE] =
      g_signal_new ("remove", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_STRUCT_OFFSET (GstPinosSocketSinkClass, remove), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 2, G_TYPE_SOCKET, G_TYPE_BOOLEAN);

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos FD sink", "Sink/Video",
      "Send data to pinos clients", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_socket_sink_template));

  gstbasesink_class->set_caps = gst_pinos_socket_sink_setcaps;
  gstbasesink_class->propose_allocation = gst_pinos_socket_sink_propose_allocation;
  gstbasesink_class->start = gst_pinos_socket_sink_start;
  gstbasesink_class->stop = gst_pinos_socket_sink_stop;
  gstbasesink_class->render = gst_pinos_socket_sink_render;

  klass->add = GST_DEBUG_FUNCPTR (gst_pinos_socket_sink_add);
  klass->remove = GST_DEBUG_FUNCPTR (gst_pinos_socket_sink_remove);

  fdids_quark = g_quark_from_static_string ("GstPinosSocketSinkFDIds");
  orig_buffer_quark = g_quark_from_static_string ("GstPinosSocketSinkOrigBuffer");

  GST_DEBUG_CATEGORY_INIT (pinos_socket_sink_debug, "pinossocketsink", 0,
      "Pinos Socket Sink");
}

static void
gst_pinos_socket_sink_init (GstPinosSocketSink * this)
{
  this->hash = g_hash_table_new (g_direct_hash, g_direct_equal);
  this->cache = gst_burst_cache_new (sizeof (MyReader));
  this->allocator = gst_tmpfile_allocator_new ();
  this->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
}
