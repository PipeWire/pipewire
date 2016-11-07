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

#if 0
#define PINOS_DEBUG_MESSAGE(format,args...) pinos_log_debug(stderr, format,##args)
#else
#define PINOS_DEBUG_MESSAGE(format,args...)
#endif

static bool
read_length (uint8_t * data, unsigned int size, size_t * length, size_t * skip)
{
  uint8_t b;

  /* start reading the length, we need this to skip to the data later */
  *length = *skip = 0;
  do {
    if (*skip >= size)
      return false;

    b = data[(*skip)++];
    *length = (*length << 7) | (b & 0x7f);
  } while (b & 0x80);

  /* check remaining command size */
  if (size - *skip < *length)
    return false;

  return true;
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

  p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosMessageUseBuffers));
  if (cmd->buffers)
    cmd->buffers = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->buffers), PinosMessageMemRef);
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
connection_add_message (PinosConnection *conn, PinosMessageType type, size_t size)
{
  uint8_t *p;
  unsigned int plen;
  ConnectionBuffer *buf = &conn->out;

  plen = 1;
  while (size >> (7 * plen))
    plen++;

  /* 1 for cmd, plen for size and size for payload */
  p = connection_ensure_size (conn, buf, 1 + plen + size);

  buf->type = type;
  buf->offset = buf->buffer_size;
  buf->buffer_size += 1 + plen + size;

  *p++ = type;
  /* write length */
  while (plen) {
    plen--;
    *p++ = ((plen > 0) ? 0x80 : 0) | ((size >> (7 * plen)) & 0x7f);
  }
  return p;
}

static void
connection_add_node_update (PinosConnection *conn, PinosMessageNodeUpdate *nu)
{
  size_t len;
  void *p;
  PinosMessageNodeUpdate *d;

  /* calc len */
  len = sizeof (PinosMessageNodeUpdate);
  len += pinos_serialize_props_get_size (nu->props);

  p = connection_add_message (conn, PINOS_MESSAGE_NODE_UPDATE, len);
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
connection_add_port_update (PinosConnection *conn, PinosMessagePortUpdate *pu)
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

  p = connection_add_message (conn, PINOS_MESSAGE_PORT_UPDATE, len);
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
connection_add_set_format (PinosConnection *conn, PinosMessageSetFormat *sf)
{
  size_t len;
  void *p;

  /* calculate length */
  /* port_id + format + mask  */
  len = sizeof (PinosMessageSetFormat) + pinos_serialize_format_get_size (sf->format);
  p = connection_add_message (conn, PINOS_MESSAGE_SET_FORMAT, len);
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
connection_add_use_buffers (PinosConnection *conn, PinosMessageUseBuffers *ub)
{
  size_t len;
  int i;
  PinosMessageUseBuffers *d;
  PinosMessageMemRef *mr;

  /* calculate length */
  len = sizeof (PinosMessageUseBuffers);
  len += ub->n_buffers * sizeof (PinosMessageMemRef);

  d = connection_add_message (conn, PINOS_MESSAGE_USE_BUFFERS, len);
  memcpy (d, ub, sizeof (PinosMessageUseBuffers));

  mr = SPA_MEMBER (d, sizeof (PinosMessageUseBuffers), void);

  if (d->n_buffers)
    d->buffers = SPA_INT_TO_PTR (SPA_PTRDIFF (mr, d));
  else
    d->buffers = 0;

  for (i = 0; i < ub->n_buffers; i++)
    memcpy (&mr[i], &ub->buffers[i], sizeof (PinosMessageMemRef));
}

static void
connection_add_node_event (PinosConnection *conn, PinosMessageNodeEvent *ev)
{
  size_t len;
  void *p;
  PinosMessageNodeEvent *d;

  /* calculate length */
  len = sizeof (PinosMessageNodeEvent);
  len += ev->event->size;

  p = connection_add_message (conn, PINOS_MESSAGE_NODE_EVENT, len);
  memcpy (p, ev, sizeof (PinosMessageNodeEvent));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeEvent), void);
  d->event = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, ev->event, ev->event->size);
}

static void
connection_add_node_command (PinosConnection *conn, PinosMessageNodeCommand *cm)
{
  size_t len;
  void *p;
  PinosMessageNodeCommand *d;

  /* calculate length */
  len = sizeof (PinosMessageNodeCommand);
  len += cm->command->size;

  p = connection_add_message (conn, PINOS_MESSAGE_NODE_COMMAND, len);
  memcpy (p, cm, sizeof (PinosMessageNodeCommand));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosMessageNodeCommand), void);
  d->command = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, cm->command, cm->command->size);
}

static void
connection_add_port_command (PinosConnection *conn, PinosMessagePortCommand *cm)
{
  size_t len;
  void *p;
  PinosMessagePortCommand *d;

  /* calculate length */
  len = sizeof (PinosMessagePortCommand);
  len += cm->command->size;

  p = connection_add_message (conn, PINOS_MESSAGE_PORT_COMMAND, len);
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

  if (len < 4)
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
  unsigned int i;

  for (i = 0; i < buf->n_fds; i++) {
    if (buf->fds[i] > 0) {
      if (close (buf->fds[i]) < 0)
        perror ("close");
    }
  }
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
  c->fd = fd;
  c->out.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->out.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->in.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.update = true;

  return c;
}

void
pinos_connection_free (PinosConnection *conn)
{
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
pinos_connection_has_next (PinosConnection *conn)
{
  size_t len, size, skip;
  uint8_t *data;
  ConnectionBuffer *buf;

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

  buf->type = *data;
  data++;
  size--;

  if (!read_length (data, size, &len, &skip)) {
    connection_ensure_size (conn, buf, len + skip);
    buf->update = true;
    goto again;
  }
  buf->size = len;
  buf->data = data + skip;
  buf->offset += 1 + skip;

  return true;
}

PinosMessageType
pinos_connection_get_type (PinosConnection *conn)
{
  spa_return_val_if_fail (conn != NULL, PINOS_MESSAGE_INVALID);

  return conn->in.type;
}

bool
pinos_connection_parse_message (PinosConnection *conn,
                                void            *message)
{
  spa_return_val_if_fail (conn != NULL, false);

  switch (conn->in.type) {
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
      if (conn->in.size < sizeof (PinosMessageAddMem))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageAddMem));
      break;

    case PINOS_MESSAGE_USE_BUFFERS:
      connection_parse_use_buffers (conn, message);
      break;

    case PINOS_MESSAGE_PROCESS_BUFFER:
      if (conn->in.size < sizeof (PinosMessageProcessBuffer))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageProcessBuffer));
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
      if (conn->in.size < sizeof (PinosMessageTransportUpdate))
        return false;
      memcpy (message, conn->in.data, sizeof (PinosMessageTransportUpdate));
      break;

    case PINOS_MESSAGE_INVALID:
      return false;
  }
  return true;
}

/**
 * pinos_connection_get_fd:
 * @conn: a #PinosConnection
 * @index: an index
 * @steal: steal the fd
 *
 * Get the file descriptor at @index in @conn.
 *
 * Returns: a file descriptor at @index in @conn. The file descriptor
 * is not duplicated in any way. -1 is returned on error.
 */
int
pinos_connection_get_fd (PinosConnection *conn,
                         unsigned int     index,
                         bool             close)
{
  int fd;

  spa_return_val_if_fail (conn != NULL, -1);
  spa_return_val_if_fail (index < conn->in.n_fds, -1);

  fd = conn->in.fds[index];
  if (fd < 0)
    fd = -fd;
  conn->in.fds[index] = close ? fd : -fd;

  return fd;
}

/**
 * pinos_connection_add_fd:
 * @conn: a #PinosConnection
 * @fd: a valid fd
 * @close: if the descriptor should be closed when sent
 *
 * Add the file descriptor @fd to @builder.
 *
 * Returns: the index of the file descriptor in @builder.
 */
int
pinos_connection_add_fd (PinosConnection *conn,
                         int              fd,
                         bool             close)
{
  int index, i;

  spa_return_val_if_fail (conn != NULL, -1);

  for (i = 0; i < conn->out.n_fds; i++) {
    if (conn->out.fds[i] == fd || conn->out.fds[i] == -fd)
      return i;
  }

  index = conn->out.n_fds;
  conn->out.fds[index] = close ? fd : -fd;
  conn->out.n_fds++;

  return index;
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
                              PinosMessageType  type,
                              void             *message)
{
  void *p;

  spa_return_val_if_fail (conn != NULL, false);
  spa_return_val_if_fail (message != NULL, false);

  switch (type) {
    /* C -> S */
    case PINOS_MESSAGE_NODE_UPDATE:
      connection_add_node_update (conn, message);
      break;

    case PINOS_MESSAGE_PORT_UPDATE:
      connection_add_port_update (conn, message);
      break;

    case PINOS_MESSAGE_PORT_STATUS_CHANGE:
      p = connection_add_message (conn, type, 0);
      break;

    case PINOS_MESSAGE_NODE_STATE_CHANGE:
      p = connection_add_message (conn, type, sizeof (PinosMessageNodeStateChange));
      memcpy (p, message, sizeof (PinosMessageNodeStateChange));
      break;

    /* S -> C */
    case PINOS_MESSAGE_ADD_PORT:
      p = connection_add_message (conn, type, sizeof (PinosMessageAddPort));
      memcpy (p, message, sizeof (PinosMessageAddPort));
      break;

    case PINOS_MESSAGE_REMOVE_PORT:
      p = connection_add_message (conn, type, sizeof (PinosMessageRemovePort));
      memcpy (p, message, sizeof (PinosMessageRemovePort));
      break;

    case PINOS_MESSAGE_SET_FORMAT:
      connection_add_set_format (conn, message);
      break;

    case PINOS_MESSAGE_SET_PROPERTY:
      pinos_log_warn ("implement builder of %d", type);
      break;

    /* bidirectional */
    case PINOS_MESSAGE_ADD_MEM:
      p = connection_add_message (conn, type, sizeof (PinosMessageAddMem));
      memcpy (p, message, sizeof (PinosMessageAddMem));
      break;

    case PINOS_MESSAGE_USE_BUFFERS:
      connection_add_use_buffers (conn, message);
      break;

    case PINOS_MESSAGE_PROCESS_BUFFER:
      p = connection_add_message (conn, type, sizeof (PinosMessageProcessBuffer));
      memcpy (p, message, sizeof (PinosMessageProcessBuffer));
      break;

    case PINOS_MESSAGE_NODE_EVENT:
      connection_add_node_event (conn, message);
      break;

    case PINOS_MESSAGE_NODE_COMMAND:
      connection_add_node_command (conn, message);
      break;

    case PINOS_MESSAGE_PORT_COMMAND:
      connection_add_port_command (conn, message);
      break;

    case PINOS_MESSAGE_TRANSPORT_UPDATE:
      p = connection_add_message (conn, type, sizeof (PinosMessageTransportUpdate));
      memcpy (p, message, sizeof (PinosMessageTransportUpdate));
      break;

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
    len = sendmsg (conn->fd, &msg, 0);
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
