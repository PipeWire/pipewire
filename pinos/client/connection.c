/* Simple Plugin API
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include "connection.h"
#include "serialize.h"
#include "log.h"

#define MAX_BUFFER_SIZE 1024
#define MAX_FDS 28

typedef struct {
  uint8_t         *buffer_data;
  size_t           buffer_size;
  size_t           buffer_maxsize;
  int              fds[MAX_FDS];
  unsigned int     n_fds;

  uint32_t         dest_id;
  PinosMessageType type;
  off_t            offset;
  void            *data;
  size_t           size;

  bool             update;
} ConnectionBuffer;

struct _PinosConnection {
  ConnectionBuffer in, out;
  int fd;
};

static int
connection_get_fd (PinosConnection *conn,
                   int              index)
{
  if (index < 0 || index >= conn->in.n_fds)
    return -1;

  return conn->in.fds[index];
}

static int
connection_add_fd (PinosConnection *conn,
                   int              fd)
{
  int index, i;

  for (i = 0; i < conn->out.n_fds; i++) {
    if (conn->out.fds[i] == fd)
      return i;
  }

  index = conn->out.n_fds;
  conn->out.fds[index] = fd;
  conn->out.n_fds++;

  return index;
}

#if 0
#define PINOS_DEBUG_MESSAGE(format,args...) pinos_log_debug(stderr, format,##args)
#else
#define PINOS_DEBUG_MESSAGE(format,args...)
#endif

static void
connection_parse_client_update (PinosConnection *conn, PinosMessageClientUpdate *m)
{
  void *p;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageClientUpdate));
  if (m->props)
    m->props = pinos_serialize_dict_deserialize (p, SPA_PTR_TO_INT (m->props));
}

static void
connection_parse_notify_global (PinosConnection *conn, PinosMessageNotifyGlobal *ng)
{
  void *p;

  p = conn->in.data;
  memcpy (ng, p, sizeof (PinosMessageNotifyGlobal));
  if (ng->type)
    ng->type = SPA_MEMBER (p, SPA_PTR_TO_INT (ng->type), const char);
}

static void
connection_parse_create_node (PinosConnection *conn, PinosMessageCreateNode *m)
{
  void *p;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageCreateNode));
  if (m->factory_name)
    m->factory_name = SPA_MEMBER (p, SPA_PTR_TO_INT (m->factory_name), const char);
  if (m->name)
    m->name = SPA_MEMBER (p, SPA_PTR_TO_INT (m->name), const char);
  if (m->props)
    m->props = pinos_serialize_dict_deserialize (p, SPA_PTR_TO_INT (m->props));
}

static void
connection_parse_create_client_node (PinosConnection *conn, PinosMessageCreateClientNode *m)
{
  void *p;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageCreateClientNode));
  if (m->name)
    m->name = SPA_MEMBER (p, SPA_PTR_TO_INT (m->name), const char);
  if (m->props)
    m->props = pinos_serialize_dict_deserialize (p, SPA_PTR_TO_INT (m->props));
}

static void
connection_parse_core_info (PinosConnection *conn, PinosMessageCoreInfo *m)
{
  void *p;
  PinosCoreInfo *di;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageCoreInfo));
  if (m->info) {
    m->info = SPA_MEMBER (p, SPA_PTR_TO_INT (m->info), PinosCoreInfo);
    di = m->info;

    if (m->info->user_name)
      m->info->user_name = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->user_name), const char);
    if (m->info->host_name)
      m->info->host_name = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->host_name), const char);
    if (m->info->version)
      m->info->version = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->version), const char);
    if (m->info->name)
      m->info->name = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->name), const char);
    if (m->info->props)
      m->info->props = pinos_serialize_dict_deserialize (di, SPA_PTR_TO_INT (m->info->props));
  }
}

static void
connection_parse_module_info (PinosConnection *conn, PinosMessageModuleInfo *m)
{
  void *p;
  PinosModuleInfo *di;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageModuleInfo));
  if (m->info) {
    m->info = SPA_MEMBER (p, SPA_PTR_TO_INT (m->info), PinosModuleInfo);
    di = m->info;
    if (m->info->name)
      m->info->name = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->name), const char);
    if (m->info->filename)
      m->info->filename = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->filename), const char);
    if (m->info->args)
      m->info->args = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->args), const char);
    if (m->info->props)
      m->info->props = pinos_serialize_dict_deserialize (di, SPA_PTR_TO_INT (m->info->props));
  }
}

static void
connection_parse_node_info (PinosConnection *conn, PinosMessageNodeInfo *m)
{
  void *p;
  PinosNodeInfo *di;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageNodeInfo));
  if (m->info) {
    m->info = SPA_MEMBER (p, SPA_PTR_TO_INT (m->info), PinosNodeInfo);
    di = m->info;

    if (m->info->name)
      m->info->name = SPA_MEMBER (di, SPA_PTR_TO_INT (m->info->name), const char);
    if (m->info->props)
      m->info->props = pinos_serialize_dict_deserialize (di, SPA_PTR_TO_INT (m->info->props));
  }
}

static void
connection_parse_client_info (PinosConnection *conn, PinosMessageClientInfo *m)
{
  void *p;
  PinosClientInfo *di;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageClientInfo));
  if (m->info) {
    m->info = SPA_MEMBER (p, SPA_PTR_TO_INT (m->info), PinosClientInfo);
    di = m->info;
    if (m->info->props)
      m->info->props = pinos_serialize_dict_deserialize (di, SPA_PTR_TO_INT (m->info->props));
  }
}

static void
connection_parse_link_info (PinosConnection *conn, PinosMessageLinkInfo *m)
{
  void *p;

  p = conn->in.data;
  memcpy (m, p, sizeof (PinosMessageLinkInfo));
  if (m->info)
    m->info = SPA_MEMBER (p, SPA_PTR_TO_INT (m->info), PinosLinkInfo);
}

static void
connection_parse_node_update (PinosConnection *conn, PinosMessageNodeUpdate *nu)
{
  memcpy (nu, conn->in.data, sizeof (PinosMessageNodeUpdate));
  if (nu->props)
    nu->props = pinos_serialize_props_deserialize (conn->in.data, SPA_PTR_TO_INT (nu->props));
}

static void
connection_parse_port_update (PinosConnection *conn, PinosMessagePortUpdate *pu)
{
  void *p;
  unsigned int i;

  memcpy (pu, conn->in.data, sizeof (PinosMessagePortUpdate));

  p = conn->in.data;

  if (pu->possible_formats)
    pu->possible_formats = SPA_MEMBER (p,
                        SPA_PTR_TO_INT (pu->possible_formats), SpaFormat *);
  for (i = 0; i < pu->n_possible_formats; i++) {
    if (pu->possible_formats[i]) {
      pu->possible_formats[i] = pinos_serialize_format_deserialize (p,
                          SPA_PTR_TO_INT (pu->possible_formats[i]));
    }
  }
  if (pu->format)
    pu->format = pinos_serialize_format_deserialize (p, SPA_PTR_TO_INT (pu->format));
  if (pu->props)
    pu->props = pinos_serialize_props_deserialize (p, SPA_PTR_TO_INT (pu->props));
  if (pu->info)
    pu->info = pinos_serialize_port_info_deserialize (p, SPA_PTR_TO_INT (pu->info));
}

static void
connection_parse_set_format (PinosConnection *conn, PinosMessageSetFormat *cmd)
{
  memcpy (cmd, conn->in.data, sizeof (PinosMessageSetFormat));
  if (cmd->format)
    cmd->format = pinos_serialize_format_deserialize (conn->in.data, SPA_PTR_TO_INT (cmd->format));
}

static void
connection_parse_use_buffers (PinosConnection *conn, PinosMessageUseBuffers *cmd)
{
  void *p;
  unsigned int i;

  p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosMessageUseBuffers));
  if (cmd->buffers)
    cmd->buffers = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->buffers), PinosMessageBuffer);

  for (i = 0; i < cmd->n_buffers; i++) {
    if (cmd->buffers[i].buffer)
      cmd->buffers[i].buffer = pinos_serialize_buffer_deserialize (conn->in.data,
          SPA_PTR_TO_INT (cmd->buffers[i].buffer));
  }
}

static void
connection_parse_node_event (PinosConnection *conn, PinosMessageNodeEvent *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosMessageNodeEvent));
  if (cmd->event)
    cmd->event = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->event), SpaNodeEvent);
}

static void
connection_parse_node_command (PinosConnection *conn, PinosMessageNodeCommand *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosMessageNodeCommand));
  if (cmd->command)
    cmd->command = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->command), SpaNodeCommand);
}

static void
connection_parse_port_command (PinosConnection *conn, PinosMessagePortCommand *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosMessagePortCommand));
  if (cmd->command)
    cmd->command = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->command), SpaNodeCommand);
}

static void *
connection_ensure_size (PinosConnection *conn, ConnectionBuffer *buf, size_t size)
{
  if (buf->buffer_size + size > buf->buffer_maxsize) {
    buf->buffer_maxsize = buf->buffer_size + MAX_BUFFER_SIZE * ((size + MAX_BUFFER_SIZE-1) / MAX_BUFFER_SIZE);
    pinos_log_warn ("connection %p: resize buffer to %zd", conn, buf->buffer_maxsize);
    buf->buffer_data = realloc (buf->buffer_data, buf->buffer_maxsize);
  }
  return (uint8_t *) buf->buffer_data + buf->buffer_size;
}

static void *
connection_add_message (PinosConnection *conn,
                        uint32_t         dest_id,
                        PinosMessageType type,
                        size_t           size)
{
  uint32_t *p;
  ConnectionBuffer *buf = &conn->out;

  /* 4 for dest_id, 2 for cmd, 2 for size and size for payload */
  p = connection_ensure_size (conn, buf, 8 + size);

  buf->type = type;
  buf->offset = buf->buffer_size;
  buf->buffer_size += 8 + size;

  *p++ = dest_id;
  *p++ = (type << 16) | (size & 0xffff);

  return p;
}

static void
connection_add_client_update (PinosConnection *conn,
                              uint32_t         dest_id,
                              PinosMessageClientUpdate *m)
{
  size_t len;
  void *p;
  PinosMessageClientUpdate *d;

  len = sizeof (PinosMessageClientUpdate);
  len += pinos_serialize_dict_get_size (m->props);

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_CLIENT_UPDATE, len);
  memcpy (p, m, sizeof (PinosMessageClientUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageClientUpdate), void);
  if (m->props) {
    len = pinos_serialize_dict_serialize (p, m->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  }
}

static void
connection_add_notify_global (PinosConnection *conn,
                              uint32_t         dest_id,
                              PinosMessageNotifyGlobal *m)
{
  size_t len;
  void *p;
  PinosMessageNotifyGlobal *d;

  /* calc len */
  len = sizeof (PinosMessageNotifyGlobal);
  len += m->type ? strlen (m->type) + 1 : 0;

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_NOTIFY_GLOBAL, len);
  memcpy (p, m, sizeof (PinosMessageNotifyGlobal));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNotifyGlobal), void);
  if (m->type) {
    strcpy (p, m->type);
    d->type = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  }
}

static void
connection_add_create_node (PinosConnection *conn, uint32_t dest_id, PinosMessageCreateNode *m)
{
  size_t len, slen;
  void *p;
  PinosMessageCreateNode *d;

  /* calc len */
  len = sizeof (PinosMessageCreateNode);
  len += m->factory_name ? strlen (m->factory_name) + 1 : 0;
  len += m->name ? strlen (m->name) + 1 : 0;
  len += pinos_serialize_dict_get_size (m->props);

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_CREATE_NODE, len);
  memcpy (p, m, sizeof (PinosMessageCreateNode));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageCreateNode), void);
  if (m->factory_name) {
    slen = strlen (m->factory_name) + 1;
    memcpy (p, m->factory_name, slen);
    d->factory_name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p += slen;
  }
  if (m->name) {
    slen = strlen (m->name) + 1;
    memcpy (p, m->name, slen);
    d->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p += slen;
  }
  if (m->props) {
    len = pinos_serialize_dict_serialize (p, m->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  }
}

static void
connection_add_create_client_node (PinosConnection *conn, uint32_t dest_id, PinosMessageCreateClientNode *m)
{
  size_t len, slen;
  void *p;
  PinosMessageCreateClientNode *d;

  /* calc len */
  len = sizeof (PinosMessageCreateClientNode);
  len += m->name ? strlen (m->name) + 1 : 0;
  len += pinos_serialize_dict_get_size (m->props);

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_CREATE_CLIENT_NODE, len);
  memcpy (p, m, sizeof (PinosMessageCreateClientNode));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageCreateClientNode), void);
  if (m->name) {
    slen = strlen (m->name) + 1;
    memcpy (p, m->name, slen);
    d->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p += slen;
  }
  if (m->props) {
    len = pinos_serialize_dict_serialize (p, m->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  }
}

static void
connection_add_core_info (PinosConnection *conn, uint32_t dest_id, PinosMessageCoreInfo *m)
{
  size_t len, slen;
  void *p;
  PinosMessageCoreInfo *d;

  /* calc len */
  len = sizeof (PinosMessageCoreInfo);
  if (m->info) {
    len += sizeof (PinosCoreInfo);
    len += m->info->user_name ? strlen (m->info->user_name) + 1 : 0;
    len += m->info->host_name ? strlen (m->info->host_name) + 1 : 0;
    len += m->info->version ? strlen (m->info->version) + 1 : 0;
    len += m->info->name ? strlen (m->info->name) + 1 : 0;
    len += pinos_serialize_dict_get_size (m->info->props);
  }

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_CORE_INFO, len);
  memcpy (p, m, sizeof (PinosMessageCoreInfo));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageCoreInfo), void);
  if (m->info) {
    PinosCoreInfo *di;

    memcpy (p, m->info, sizeof (PinosCoreInfo));
    di = p;

    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

    p = SPA_MEMBER (p, sizeof (PinosCoreInfo), void);
    if (m->info->user_name) {
      slen = strlen (m->info->user_name) + 1;
      memcpy (p, m->info->user_name, slen);
      di->user_name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->host_name) {
      slen = strlen (m->info->host_name) + 1;
      memcpy (p, m->info->host_name, slen);
      di->host_name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->version) {
      slen = strlen (m->info->version) + 1;
      memcpy (p, m->info->version, slen);
      di->version = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->name) {
      slen = strlen (m->info->name) + 1;
      memcpy (p, m->info->name, slen);
      di->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->props) {
      len = pinos_serialize_dict_serialize (p, m->info->props);
      di->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
    }
  }
}

static void
connection_add_module_info (PinosConnection *conn, uint32_t dest_id, PinosMessageModuleInfo *m)
{
  size_t len, slen;
  void *p;
  PinosMessageModuleInfo *d;

  /* calc len */
  len = sizeof (PinosMessageModuleInfo);
  if (m->info) {
    len += sizeof (PinosModuleInfo);
    len += m->info->name ? strlen (m->info->name) + 1 : 0;
    len += m->info->filename ? strlen (m->info->filename) + 1 : 0;
    len += m->info->args ? strlen (m->info->args) + 1 : 0;
    len += pinos_serialize_dict_get_size (m->info->props);
  }

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_MODULE_INFO, len);
  memcpy (p, m, sizeof (PinosMessageModuleInfo));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageModuleInfo), void);
  if (m->info) {
    PinosModuleInfo *di;

    memcpy (p, m->info, sizeof (PinosModuleInfo));
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    di = p;

    p = SPA_MEMBER (p, sizeof (PinosModuleInfo), void);
    if (m->info->name) {
      slen = strlen (m->info->name) + 1;
      memcpy (p, m->info->name, slen);
      di->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->filename) {
      slen = strlen (m->info->filename) + 1;
      memcpy (p, m->info->filename, slen);
      di->filename = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->args) {
      slen = strlen (m->info->args) + 1;
      memcpy (p, m->info->args, slen);
      di->args = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->props) {
      len = pinos_serialize_dict_serialize (p, m->info->props);
      di->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
    }
  }
}

static void
connection_add_node_info (PinosConnection *conn, uint32_t dest_id, PinosMessageNodeInfo *m)
{
  size_t len, slen;
  void *p;
  PinosMessageNodeInfo *d;

  /* calc len */
  len = sizeof (PinosMessageNodeInfo);
  if (m->info) {
    len += sizeof (PinosNodeInfo);
    len += m->info->name ? strlen (m->info->name) + 1 : 0;
    len += pinos_serialize_dict_get_size (m->info->props);
  }

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_NODE_INFO, len);
  memcpy (p, m, sizeof (PinosMessageNodeInfo));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeInfo), void);
  if (m->info) {
    PinosNodeInfo *di;

    memcpy (p, m->info, sizeof (PinosNodeInfo));
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    di = p;

    p = SPA_MEMBER (p, sizeof (PinosNodeInfo), void);
    if (m->info->name) {
      slen = strlen (m->info->name) + 1;
      memcpy (p, m->info->name, slen);
      di->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
      p += slen;
    }
    if (m->info->props) {
      len = pinos_serialize_dict_serialize (p, m->info->props);
      di->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
    }
  }
}

static void
connection_add_client_info (PinosConnection *conn, uint32_t dest_id, PinosMessageClientInfo *m)
{
  size_t len;
  void *p;
  PinosMessageClientInfo *d;

  /* calc len */
  len = sizeof (PinosMessageClientInfo);
  if (m->info) {
    len += sizeof (PinosClientInfo);
    len += pinos_serialize_dict_get_size (m->info->props);
  }

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_CLIENT_INFO, len);
  memcpy (p, m, sizeof (PinosMessageClientInfo));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageClientInfo), void);
  if (m->info) {
    PinosClientInfo *di;

    memcpy (p, m->info, sizeof (PinosClientInfo));
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    di = p;

    p = SPA_MEMBER (p, sizeof (PinosClientInfo), void);
    if (m->info->props) {
      len = pinos_serialize_dict_serialize (p, m->info->props);
      di->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, di));
    }
  }
}

static void
connection_add_link_info (PinosConnection *conn, uint32_t dest_id, PinosMessageLinkInfo *m)
{
  size_t len;
  void *p;
  PinosMessageLinkInfo *d;

  /* calc len */
  len = sizeof (PinosMessageLinkInfo);
  if (m->info) {
    len += sizeof (PinosLinkInfo);
  }

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_LINK_INFO, len);
  memcpy (p, m, sizeof (PinosMessageLinkInfo));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageLinkInfo), void);
  if (m->info) {
    memcpy (p, m->info, sizeof (PinosLinkInfo));
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  }
}

static void
connection_add_node_update (PinosConnection *conn, uint32_t dest_id, PinosMessageNodeUpdate *nu)
{
  size_t len;
  void *p;
  PinosMessageNodeUpdate *d;

  /* calc len */
  len = sizeof (PinosMessageNodeUpdate);
  len += pinos_serialize_props_get_size (nu->props);

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_NODE_UPDATE, len);
  memcpy (p, nu, sizeof (PinosMessageNodeUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeUpdate), void);
  if (nu->props) {
    len = pinos_serialize_props_serialize (p, nu->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  } else {
    d->props = 0;
  }
}

static void
connection_add_port_update (PinosConnection *conn, uint32_t dest_id, PinosMessagePortUpdate *pu)
{
  size_t len;
  void *p;
  int i;
  SpaFormat **bfa;
  PinosMessagePortUpdate *d;

  /* calc len */
  len = sizeof (PinosMessagePortUpdate);
  len += pu->n_possible_formats * sizeof (SpaFormat *);
  for (i = 0; i < pu->n_possible_formats; i++) {
    len += pinos_serialize_format_get_size (pu->possible_formats[i]);
  }
  len += pinos_serialize_format_get_size (pu->format);
  len += pinos_serialize_props_get_size (pu->props);
  if (pu->info)
    len += pinos_serialize_port_info_get_size (pu->info);

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_PORT_UPDATE, len);
  memcpy (p, pu, sizeof (PinosMessagePortUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessagePortUpdate), void);
  bfa = p;
  if (pu->n_possible_formats)
    d->possible_formats = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  else
    d->possible_formats = 0;

  p = SPA_MEMBER (p, sizeof (SpaFormat*) * pu->n_possible_formats, void);

  for (i = 0; i < pu->n_possible_formats; i++) {
    len = pinos_serialize_format_serialize (p, pu->possible_formats[i]);
    bfa[i] = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  }
  if (pu->format) {
    len = pinos_serialize_format_serialize (p, pu->format);
    d->format = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->format = 0;
  }
  if (pu->props) {
    len = pinos_serialize_props_serialize (p, pu->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->props = 0;
  }
  if (pu->info) {
    len = pinos_serialize_port_info_serialize (p, pu->info);
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->info = 0;
  }
}

static void
connection_add_set_format (PinosConnection *conn, uint32_t dest_id, PinosMessageSetFormat *sf)
{
  size_t len;
  void *p;

  /* calculate length */
  /* port_id + format + mask  */
  len = sizeof (PinosMessageSetFormat) + pinos_serialize_format_get_size (sf->format);
  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_SET_FORMAT, len);
  memcpy (p, sf, sizeof (PinosMessageSetFormat));
  sf = p;

  p = SPA_MEMBER (sf, sizeof (PinosMessageSetFormat), void);
  if (sf->format) {
    len = pinos_serialize_format_serialize (p, sf->format);
    sf->format = SPA_INT_TO_PTR (SPA_PTRDIFF (p, sf));
  } else
    sf->format = 0;
}

static void
connection_add_use_buffers (PinosConnection *conn, uint32_t dest_id, PinosMessageUseBuffers *ub)
{
  size_t len;
  int i;
  PinosMessageUseBuffers *d;
  PinosMessageBuffer *b;
  void *p;

  /* calculate length */
  len = sizeof (PinosMessageUseBuffers);
  len += ub->n_buffers * sizeof (PinosMessageBuffer);
  for (i = 0; i < ub->n_buffers; i++)
    len += pinos_serialize_buffer_get_size (ub->buffers[i].buffer);

  d = connection_add_message (conn, dest_id, PINOS_MESSAGE_USE_BUFFERS, len);
  memcpy (d, ub, sizeof (PinosMessageUseBuffers));

  b = SPA_MEMBER (d, sizeof (PinosMessageUseBuffers), void);
  p = SPA_MEMBER (b, ub->n_buffers * sizeof (PinosMessageBuffer), void);

  if (d->n_buffers)
    d->buffers = SPA_INT_TO_PTR (SPA_PTRDIFF (b, d));
  else
    d->buffers = 0;

  for (i = 0; i < ub->n_buffers; i++) {
    memcpy (&b[i], &ub->buffers[i], sizeof (PinosMessageBuffer));
    len = pinos_serialize_buffer_serialize (p, b[i].buffer);
    b[i].buffer = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p += len;
  }
}

static void
connection_add_node_event (PinosConnection *conn, uint32_t dest_id, PinosMessageNodeEvent *ev)
{
  size_t len;
  void *p;
  PinosMessageNodeEvent *d;

  /* calculate length */
  len = sizeof (PinosMessageNodeEvent);
  len += ev->event->size;

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_NODE_EVENT, len);
  memcpy (p, ev, sizeof (PinosMessageNodeEvent));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeEvent), void);
  d->event = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, ev->event, ev->event->size);
}

static void
connection_add_node_command (PinosConnection *conn, uint32_t dest_id, PinosMessageNodeCommand *cm)
{
  size_t len;
  void *p;
  PinosMessageNodeCommand *d;

  /* calculate length */
  len = sizeof (PinosMessageNodeCommand);
  len += cm->command->size;

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_NODE_COMMAND, len);
  memcpy (p, cm, sizeof (PinosMessageNodeCommand));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeCommand), void);
  d->command = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, cm->command, cm->command->size);
}

static void
connection_add_port_command (PinosConnection *conn, uint32_t dest_id, PinosMessagePortCommand *cm)
{
  size_t len;
  void *p;
  PinosMessagePortCommand *d;

  /* calculate length */
  len = sizeof (PinosMessagePortCommand);
  len += cm->command->size;

  p = connection_add_message (conn, dest_id, PINOS_MESSAGE_PORT_COMMAND, len);
  memcpy (p, cm, sizeof (PinosMessagePortCommand));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessagePortCommand), void);
  d->command = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, cm->command, cm->command->size);
}

static bool
refill_buffer (PinosConnection *conn, ConnectionBuffer *buf)
{
  ssize_t len;
  struct cmsghdr *cmsg;
  struct msghdr msg = {0};
  struct iovec iov[1];
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];

  iov[0].iov_base = buf->buffer_data + buf->buffer_size;
  iov[0].iov_len = buf->buffer_maxsize - buf->buffer_size;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);
  msg.msg_flags = MSG_CMSG_CLOEXEC;

  while (true) {
    len = recvmsg (conn->fd, &msg, msg.msg_flags);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto recv_error;
    }
    break;
  }

  if (len < 8)
    return false;

  buf->buffer_size += len;

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    buf->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (buf->fds, CMSG_DATA (cmsg), buf->n_fds * sizeof (int));
  }
  PINOS_DEBUG_MESSAGE ("connection %p: %d read %zd bytes and %d fds", conn, conn->fd, len, buf->n_fds);

  return true;

  /* ERRORS */
recv_error:
  {
    pinos_log_error ("could not recvmsg on fd %d: %s", conn->fd, strerror (errno));
    return false;
  }
}

static void
clear_buffer (ConnectionBuffer *buf)
{
  buf->n_fds = 0;
  buf->type = PINOS_MESSAGE_INVALID;
  buf->offset = 0;
  buf->size = 0;
  buf->buffer_size = 0;
}

PinosConnection *
pinos_connection_new (int fd)
{
  PinosConnection *c;

  c = calloc (1, sizeof (PinosConnection));
  pinos_log_debug ("connection %p: new", c);
  c->fd = fd;
  c->out.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->out.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->in.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.update = true;

  return c;
}

void
pinos_connection_destroy (PinosConnection *conn)
{
  pinos_log_debug ("connection %p: destroy", conn);
  free (conn->out.buffer_data);
  free (conn->in.buffer_data);
  free (conn);
}

/**
 * pinos_connection_has_next:
 * @iter: a #PinosConnection
 *
 * Move to the next packet in @conn.
 *
 * Returns: %true if more packets are available.
 */
bool
pinos_connection_get_next (PinosConnection  *conn,
                           PinosMessageType *type,
                           uint32_t         *dest_id,
                           size_t           *sz)
{
  size_t len, size;
  uint8_t *data;
  ConnectionBuffer *buf;
  uint32_t *p;

  spa_return_val_if_fail (conn != NULL, false);

  buf = &conn->in;

  /* move to next packet */
  buf->offset += buf->size;

again:
  if (buf->update) {
    refill_buffer (conn, buf);
    buf->update = false;
  }

  /* now read packet */
  data = buf->buffer_data;
  size = buf->buffer_size;

  if (buf->offset >= size) {
    clear_buffer (buf);
    buf->update = true;
    return false;
  }

  data += buf->offset;
  size -= buf->offset;

  if (size < 8) {
    connection_ensure_size (conn, buf, 8);
    buf->update = true;
    goto again;
  }
  p = (uint32_t *) data;
  data += 8;
  size -= 8;

  buf->dest_id = p[0];
  buf->type = p[1] >> 16;
  len = p[1] & 0xffff;

  if (len > size) {
    connection_ensure_size (conn, buf, len);
    buf->update = true;
    goto again;
  }
  buf->size = len;
  buf->data = data;
  buf->offset += 8;

  *type = buf->type;
  *dest_id = buf->dest_id;
  *sz = buf->size;

  return true;
}

bool
pinos_connection_parse_message (PinosConnection *conn,
                                void            *message)
{
  spa_return_val_if_fail (conn != NULL, false);

  switch (conn->in.type) {
    case PINOS_MESSAGE_CLIENT_UPDATE:
      connection_parse_client_update (conn, message);
      break;

    case PINOS_MESSAGE_SYNC:
      if (conn->in.size < sizeof (PinosMessageSync))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageSync));
      break;

    case PINOS_MESSAGE_NOTIFY_DONE:
      if (conn->in.size < sizeof (PinosMessageNotifyDone))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageNotifyDone));
      break;

    case PINOS_MESSAGE_GET_REGISTRY:
      if (conn->in.size < sizeof (PinosMessageGetRegistry))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageGetRegistry));
      break;

    case PINOS_MESSAGE_BIND:
      if (conn->in.size < sizeof (PinosMessageBind))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageBind));
      break;

    case PINOS_MESSAGE_NOTIFY_GLOBAL:
      connection_parse_notify_global (conn, message);
      break;

    case PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE:
      if (conn->in.size < sizeof (PinosMessageNotifyGlobalRemove))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageNotifyGlobalRemove));
      break;

    case PINOS_MESSAGE_CREATE_NODE:
      connection_parse_create_node (conn, message);
      break;

    case PINOS_MESSAGE_CREATE_NODE_DONE:
      if (conn->in.size < sizeof (PinosMessageCreateNodeDone))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageCreateNodeDone));
      break;

    case PINOS_MESSAGE_CREATE_CLIENT_NODE:
      connection_parse_create_client_node (conn, message);
      break;

    case PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE:
    {
      PinosMessageCreateClientNodeDone *d = message;
      if (conn->in.size < sizeof (PinosMessageCreateClientNodeDone))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageCreateClientNodeDone));
      d->datafd = connection_get_fd (conn, d->datafd);
      break;
    }
    case PINOS_MESSAGE_DESTROY:
      if (conn->in.size < sizeof (PinosMessageDestroy))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageDestroy));
      break;

    case PINOS_MESSAGE_REMOVE_ID:
      if (conn->in.size < sizeof (PinosMessageRemoveId))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageRemoveId));
      break;

    case PINOS_MESSAGE_CORE_INFO:
      connection_parse_core_info (conn, message);
      break;

    case PINOS_MESSAGE_MODULE_INFO:
      connection_parse_module_info (conn, message);
      break;

    case PINOS_MESSAGE_NODE_INFO:
      connection_parse_node_info (conn, message);
      break;

    case PINOS_MESSAGE_CLIENT_INFO:
      connection_parse_client_info (conn, message);
      break;

    case PINOS_MESSAGE_LINK_INFO:
      connection_parse_link_info (conn, message);
      break;

    /* C -> S */
    case PINOS_MESSAGE_NODE_UPDATE:
      connection_parse_node_update (conn, message);
      break;

    case PINOS_MESSAGE_PORT_UPDATE:
      connection_parse_port_update (conn, message);
      break;

    case PINOS_MESSAGE_PORT_STATUS_CHANGE:
      pinos_log_warn ("implement iter of %d", conn->in.type);
      break;

    case PINOS_MESSAGE_NODE_STATE_CHANGE:
      if (conn->in.size < sizeof (PinosMessageNodeStateChange))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageNodeStateChange));
      break;

    /* S -> C */
    case PINOS_MESSAGE_ADD_PORT:
      if (conn->in.size < sizeof (PinosMessageAddPort))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageAddPort));
      break;

    case PINOS_MESSAGE_REMOVE_PORT:
      if (conn->in.size < sizeof (PinosMessageRemovePort))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageRemovePort));
      break;

    case PINOS_MESSAGE_SET_FORMAT:
      connection_parse_set_format (conn, message);
      break;

    case PINOS_MESSAGE_SET_PROPERTY:
      pinos_log_warn ("implement iter of %d", conn->in.type);
      break;

    /* bidirectional */
    case PINOS_MESSAGE_ADD_MEM:
    {
      PinosMessageAddMem *d = message;
      if (conn->in.size < sizeof (PinosMessageAddMem))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageAddMem));
      d->memfd = connection_get_fd (conn, d->memfd);
      break;
    }

    case PINOS_MESSAGE_USE_BUFFERS:
      connection_parse_use_buffers (conn, message);
      break;

    case PINOS_MESSAGE_NODE_EVENT:
      connection_parse_node_event (conn, message);
      break;

    case PINOS_MESSAGE_NODE_COMMAND:
      connection_parse_node_command (conn, message);
      break;

    case PINOS_MESSAGE_PORT_COMMAND:
      connection_parse_port_command (conn, message);
      break;

    case PINOS_MESSAGE_TRANSPORT_UPDATE:
    {
      PinosMessageTransportUpdate *d = message;
      if (conn->in.size < sizeof (PinosMessageTransportUpdate))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageTransportUpdate));
      d->memfd = connection_get_fd (conn, d->memfd);
      break;
    }

    case PINOS_MESSAGE_INVALID:
      return false;
  }
  return true;
}

/**
 * pinos_connection_add_message:
 * @conn: a #PinosConnection
 * @type: a #PinosMessageType
 * @message: a message
 *
 * Add a @cmd to @conn with data from @message.
 *
 * Returns: %true on success.
 */
bool
pinos_connection_add_message (PinosConnection  *conn,
                              uint32_t          dest_id,
                              PinosMessageType  type,
                              void             *message)
{
  void *p;

  spa_return_val_if_fail (conn != NULL, false);
  spa_return_val_if_fail (message != NULL, false);

  switch (type) {
    case PINOS_MESSAGE_CLIENT_UPDATE:
      connection_add_client_update (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_SYNC:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageSync));
      memcpy (p, message, sizeof (PinosMessageSync));
      break;

    case PINOS_MESSAGE_NOTIFY_DONE:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageNotifyDone));
      memcpy (p, message, sizeof (PinosMessageNotifyDone));
      break;

    case PINOS_MESSAGE_GET_REGISTRY:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageGetRegistry));
      memcpy (p, message, sizeof (PinosMessageGetRegistry));
      break;

    case PINOS_MESSAGE_BIND:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageBind));
      memcpy (p, message, sizeof (PinosMessageBind));
      break;

    case PINOS_MESSAGE_NOTIFY_GLOBAL:
      connection_add_notify_global (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageNotifyGlobalRemove));
      memcpy (p, message, sizeof (PinosMessageNotifyGlobalRemove));
      break;

    case PINOS_MESSAGE_CREATE_NODE:
      connection_add_create_node (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_CREATE_NODE_DONE:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageCreateNodeDone));
      memcpy (p, message, sizeof (PinosMessageCreateNodeDone));
      break;

    case PINOS_MESSAGE_CREATE_CLIENT_NODE:
      connection_add_create_client_node (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE:
    {
      PinosMessageCreateClientNodeDone *d;
      d = connection_add_message (conn, dest_id, type, sizeof (PinosMessageCreateClientNodeDone));
      memcpy (d, message, sizeof (PinosMessageCreateClientNodeDone));
      d->datafd = connection_add_fd (conn, d->datafd);
      break;
    }
    case PINOS_MESSAGE_DESTROY:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageDestroy));
      memcpy (p, message, sizeof (PinosMessageDestroy));
      break;

    case PINOS_MESSAGE_REMOVE_ID:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageRemoveId));
      memcpy (p, message, sizeof (PinosMessageRemoveId));
      break;

    case PINOS_MESSAGE_CORE_INFO:
      connection_add_core_info (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_MODULE_INFO:
      connection_add_module_info (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_NODE_INFO:
      connection_add_node_info (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_CLIENT_INFO:
      connection_add_client_info (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_LINK_INFO:
      connection_add_link_info (conn, dest_id, message);
      break;

    /* C -> S */
    case PINOS_MESSAGE_NODE_UPDATE:
      connection_add_node_update (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_PORT_UPDATE:
      connection_add_port_update (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_PORT_STATUS_CHANGE:
      p = connection_add_message (conn, dest_id, type, 0);
      break;

    case PINOS_MESSAGE_NODE_STATE_CHANGE:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageNodeStateChange));
      memcpy (p, message, sizeof (PinosMessageNodeStateChange));
      break;

    /* S -> C */
    case PINOS_MESSAGE_ADD_PORT:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageAddPort));
      memcpy (p, message, sizeof (PinosMessageAddPort));
      break;

    case PINOS_MESSAGE_REMOVE_PORT:
      p = connection_add_message (conn, dest_id, type, sizeof (PinosMessageRemovePort));
      memcpy (p, message, sizeof (PinosMessageRemovePort));
      break;

    case PINOS_MESSAGE_SET_FORMAT:
      connection_add_set_format (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_SET_PROPERTY:
      pinos_log_warn ("implement builder of %d", type);
      break;

    /* bidirectional */
    case PINOS_MESSAGE_ADD_MEM:
    {
      PinosMessageAddMem *d;
      d = connection_add_message (conn, dest_id, type, sizeof (PinosMessageAddMem));
      memcpy (d, message, sizeof (PinosMessageAddMem));
      d->memfd = connection_add_fd (conn, d->memfd);
      break;
    }

    case PINOS_MESSAGE_USE_BUFFERS:
      connection_add_use_buffers (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_NODE_EVENT:
      connection_add_node_event (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_NODE_COMMAND:
      connection_add_node_command (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_PORT_COMMAND:
      connection_add_port_command (conn, dest_id, message);
      break;

    case PINOS_MESSAGE_TRANSPORT_UPDATE:
    {
      PinosMessageTransportUpdate *d;
      d = connection_add_message (conn, dest_id, type, sizeof (PinosMessageTransportUpdate));
      memcpy (d, message, sizeof (PinosMessageTransportUpdate));
      d->memfd = connection_add_fd (conn, d->memfd);
      break;
    }

    case PINOS_MESSAGE_INVALID:
      return false;
  }
  return true;
}

bool
pinos_connection_flush (PinosConnection *conn)
{
  ssize_t len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];
  int *cm, i, fds_len;
  ConnectionBuffer *buf;

  spa_return_val_if_fail (conn != NULL, false);

  buf = &conn->out;

  if (buf->buffer_size == 0)
    return true;

  fds_len = buf->n_fds * sizeof (int);

  iov[0].iov_base = buf->buffer_data;
  iov[0].iov_len = buf->buffer_size;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  if (buf->n_fds > 0) {
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE (fds_len);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN (fds_len);
    cm = (int*)CMSG_DATA (cmsg);
    for (i = 0; i < buf->n_fds; i++)
      cm[i] = buf->fds[i] > 0 ? buf->fds[i] : -buf->fds[i];
    msg.msg_controllen = cmsg->cmsg_len;
  } else {
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
  }

  while (true) {
    len = sendmsg (conn->fd, &msg, MSG_NOSIGNAL);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto send_error;
    }
    break;
  }
  buf->buffer_size -= len;
  buf->n_fds = 0;

  PINOS_DEBUG_MESSAGE ("connection %p: %d written %zd bytes and %u fds", conn, conn->fd, len, buf->n_fds);

  return true;

  /* ERRORS */
send_error:
  {
    pinos_log_error ("could not sendmsg: %s", strerror (errno));
    return false;
  }
}

bool
pinos_connection_clear (PinosConnection *conn)
{
  spa_return_val_if_fail (conn != NULL, false);

  clear_buffer (&conn->out);
  clear_buffer (&conn->in);
  conn->in.update = true;

  return true;
}
