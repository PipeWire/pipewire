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

#define MAX_BUFFER_SIZE 1024
#define MAX_FDS 28

typedef struct {
  uint8_t        *buffer_data;
  size_t          buffer_size;
  size_t          buffer_maxsize;
  int             fds[MAX_FDS];
  unsigned int    n_fds;

  PinosControlCmd cmd;
  off_t           offset;
  void           *data;
  size_t          size;

  bool            update;
} ConnectionBuffer;

struct _PinosConnection {
  ConnectionBuffer in, out;
  int fd;
};

#if 0
#define PINOS_DEBUG_CONTROL(format,args...) g_debug(format,##args)
#else
#define PINOS_DEBUG_CONTROL(format,args...)
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
connection_parse_node_update (PinosConnection *conn, PinosControlCmdNodeUpdate *nu)
{
  memcpy (nu, conn->in.data, sizeof (PinosControlCmdNodeUpdate));
  if (nu->props)
    nu->props = pinos_serialize_props_deserialize (conn->in.data, SPA_PTR_TO_INT (nu->props));
}

static void
connection_parse_port_update (PinosConnection *conn, PinosControlCmdPortUpdate *pu)
{
  void *p;
  unsigned int i;

  memcpy (pu, conn->in.data, sizeof (PinosControlCmdPortUpdate));

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
connection_parse_set_format (PinosConnection *conn, PinosControlCmdSetFormat *cmd)
{
  memcpy (cmd, conn->in.data, sizeof (PinosControlCmdSetFormat));
  if (cmd->format)
    cmd->format = pinos_serialize_format_deserialize (conn->in.data, SPA_PTR_TO_INT (cmd->format));
}

static void
connection_parse_use_buffers (PinosConnection *conn, PinosControlCmdUseBuffers *cmd)
{
  void *p;

  p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosControlCmdUseBuffers));
  if (cmd->buffers)
    cmd->buffers = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->buffers), PinosControlMemRef);
}

static void
connection_parse_node_event (PinosConnection *conn, PinosControlCmdNodeEvent *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosControlCmdNodeEvent));
  if (cmd->event)
    cmd->event = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->event), SpaNodeEvent);
}

static void
connection_parse_node_command (PinosConnection *conn, PinosControlCmdNodeCommand *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (PinosControlCmdNodeCommand));
  if (cmd->command)
    cmd->command = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->command), SpaNodeCommand);
}

static void *
connection_ensure_size (PinosConnection *conn, ConnectionBuffer *buf, size_t size)
{
  if (buf->buffer_size + size > buf->buffer_maxsize) {
    buf->buffer_maxsize = buf->buffer_size + MAX_BUFFER_SIZE * ((size + MAX_BUFFER_SIZE-1) / MAX_BUFFER_SIZE);
    g_debug ("connection %p: resize buffer to %zd", conn, buf->buffer_maxsize);
    buf->buffer_data = realloc (buf->buffer_data, buf->buffer_maxsize);
  }
  return (uint8_t *) buf->buffer_data + buf->buffer_size;
}

static void *
connection_add_cmd (PinosConnection *conn, PinosControlCmd cmd, size_t size)
{
  uint8_t *p;
  unsigned int plen;
  ConnectionBuffer *buf = &conn->out;

  plen = 1;
  while (size >> (7 * plen))
    plen++;

  /* 1 for cmd, plen for size and size for payload */
  p = connection_ensure_size (conn, buf, 1 + plen + size);

  buf->cmd = cmd;
  buf->offset = buf->buffer_size;
  buf->buffer_size += 1 + plen + size;

  *p++ = cmd;
  /* write length */
  while (plen) {
    plen--;
    *p++ = ((plen > 0) ? 0x80 : 0) | ((size >> (7 * plen)) & 0x7f);
  }
  return p;
}

static void
connection_add_node_update (PinosConnection *conn, PinosControlCmdNodeUpdate *nu)
{
  size_t len;
  void *p;
  PinosControlCmdNodeUpdate *d;

  /* calc len */
  len = sizeof (PinosControlCmdNodeUpdate);
  len += pinos_serialize_props_get_size (nu->props);

  p = connection_add_cmd (conn, PINOS_CONTROL_CMD_NODE_UPDATE, len);
  memcpy (p, nu, sizeof (PinosControlCmdNodeUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosControlCmdNodeUpdate), void);
  if (nu->props) {
    len = pinos_serialize_props_serialize (p, nu->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  } else {
    d->props = 0;
  }
}

static void
connection_add_port_update (PinosConnection *conn, PinosControlCmdPortUpdate *pu)
{
  size_t len;
  void *p;
  int i;
  SpaFormat **bfa;
  PinosControlCmdPortUpdate *d;

  /* calc len */
  len = sizeof (PinosControlCmdPortUpdate);
  len += pu->n_possible_formats * sizeof (SpaFormat *);
  for (i = 0; i < pu->n_possible_formats; i++) {
    len += pinos_serialize_format_get_size (pu->possible_formats[i]);
  }
  len += pinos_serialize_format_get_size (pu->format);
  len += pinos_serialize_props_get_size (pu->props);
  if (pu->info)
    len += pinos_serialize_port_info_get_size (pu->info);

  p = connection_add_cmd (conn, PINOS_CONTROL_CMD_PORT_UPDATE, len);
  memcpy (p, pu, sizeof (PinosControlCmdPortUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosControlCmdPortUpdate), void);
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
connection_add_set_format (PinosConnection *conn, PinosControlCmdSetFormat *sf)
{
  size_t len;
  void *p;

  /* calculate length */
  /* port_id + format + mask  */
  len = sizeof (PinosControlCmdSetFormat) + pinos_serialize_format_get_size (sf->format);
  p = connection_add_cmd (conn, PINOS_CONTROL_CMD_SET_FORMAT, len);
  memcpy (p, sf, sizeof (PinosControlCmdSetFormat));
  sf = p;

  p = SPA_MEMBER (sf, sizeof (PinosControlCmdSetFormat), void);
  if (sf->format) {
    len = pinos_serialize_format_serialize (p, sf->format);
    sf->format = SPA_INT_TO_PTR (SPA_PTRDIFF (p, sf));
  } else
    sf->format = 0;
}

static void
connection_add_use_buffers (PinosConnection *conn, PinosControlCmdUseBuffers *ub)
{
  size_t len;
  int i;
  PinosControlCmdUseBuffers *d;
  PinosControlMemRef *mr;

  /* calculate length */
  len = sizeof (PinosControlCmdUseBuffers);
  len += ub->n_buffers * sizeof (PinosControlMemRef);

  d = connection_add_cmd (conn, PINOS_CONTROL_CMD_USE_BUFFERS, len);
  memcpy (d, ub, sizeof (PinosControlCmdUseBuffers));

  mr = SPA_MEMBER (d, sizeof (PinosControlCmdUseBuffers), void);

  if (d->n_buffers)
    d->buffers = SPA_INT_TO_PTR (SPA_PTRDIFF (mr, d));
  else
    d->buffers = 0;

  for (i = 0; i < ub->n_buffers; i++)
    memcpy (&mr[i], &ub->buffers[i], sizeof (PinosControlMemRef));
}

static void
connection_add_node_event (PinosConnection *conn, PinosControlCmdNodeEvent *ev)
{
  size_t len;
  void *p;
  PinosControlCmdNodeEvent *d;

  /* calculate length */
  len = sizeof (PinosControlCmdNodeEvent);
  len += ev->event->size;

  p = connection_add_cmd (conn, PINOS_CONTROL_CMD_NODE_EVENT, len);
  memcpy (p, ev, sizeof (PinosControlCmdNodeEvent));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosControlCmdNodeEvent), void);
  d->event = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, ev->event, ev->event->size);
}

static void
connection_add_node_command (PinosConnection *conn, PinosControlCmdNodeCommand *cm)
{
  size_t len;
  void *p;
  PinosControlCmdNodeCommand *d;

  /* calculate length */
  len = sizeof (PinosControlCmdNodeCommand);
  len += cm->command->size;

  p = connection_add_cmd (conn, PINOS_CONTROL_CMD_NODE_COMMAND, len);
  memcpy (p, cm, sizeof (PinosControlCmdNodeCommand));
  d = p;

  p = SPA_MEMBER (d, sizeof (PinosControlCmdNodeCommand), void);
  d->command = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  memcpy (p, cm->command, cm->command->size);
}

static gboolean
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
    return FALSE;

  buf->buffer_size += len;

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    buf->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (buf->fds, CMSG_DATA (cmsg), buf->n_fds * sizeof (int));
  }
  PINOS_DEBUG_CONTROL ("connection %p: %d read %zd bytes and %d fds", conn, conn->fd, len, buf->n_fds);

  return TRUE;

  /* ERRORS */
recv_error:
  {
    g_warning ("could not recvmsg on fd %d: %s", conn->fd, strerror (errno));
    return FALSE;
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
  buf->cmd = PINOS_CONTROL_CMD_INVALID;
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
 * Returns: %TRUE if more packets are available.
 */
gboolean
pinos_connection_has_next (PinosConnection *conn)
{
  size_t len, size, skip;
  uint8_t *data;
  ConnectionBuffer *buf;

  g_return_val_if_fail (conn != NULL, FALSE);

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
    return FALSE;
  }

  data += buf->offset;
  size -= buf->offset;

  buf->cmd = *data;
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

  return TRUE;
}

PinosControlCmd
pinos_connection_get_cmd (PinosConnection *conn)
{
  g_return_val_if_fail (conn != NULL, PINOS_CONTROL_CMD_INVALID);

  return conn->in.cmd;
}

gboolean
pinos_connection_parse_cmd (PinosConnection *conn,
                            gpointer         command)
{
  g_return_val_if_fail (conn != NULL, FALSE);

  switch (conn->in.cmd) {
    /* C -> S */
    case PINOS_CONTROL_CMD_NODE_UPDATE:
      connection_parse_node_update (conn, command);
      break;

    case PINOS_CONTROL_CMD_PORT_UPDATE:
      connection_parse_port_update (conn, command);
      break;

    case PINOS_CONTROL_CMD_PORT_STATUS_CHANGE:
      g_warning ("implement iter of %d", conn->in.cmd);
      break;

    case PINOS_CONTROL_CMD_NODE_STATE_CHANGE:
      if (conn->in.size < sizeof (PinosControlCmdNodeStateChange))
        return FALSE;
      memcpy (command, conn->in.data, sizeof (PinosControlCmdNodeStateChange));
      break;

    /* S -> C */
    case PINOS_CONTROL_CMD_ADD_PORT:
      if (conn->in.size < sizeof (PinosControlCmdAddPort))
        return FALSE;
      memcpy (command, conn->in.data, sizeof (PinosControlCmdAddPort));
      break;

    case PINOS_CONTROL_CMD_REMOVE_PORT:
      if (conn->in.size < sizeof (PinosControlCmdRemovePort))
        return FALSE;
      memcpy (command, conn->in.data, sizeof (PinosControlCmdRemovePort));
      break;

    case PINOS_CONTROL_CMD_SET_FORMAT:
      connection_parse_set_format (conn, command);
      break;

    case PINOS_CONTROL_CMD_SET_PROPERTY:
      g_warning ("implement iter of %d", conn->in.cmd);
      break;

    /* bidirectional */
    case PINOS_CONTROL_CMD_ADD_MEM:
      if (conn->in.size < sizeof (PinosControlCmdAddMem))
        return FALSE;
      memcpy (command, conn->in.data, sizeof (PinosControlCmdAddMem));
      break;

    case PINOS_CONTROL_CMD_USE_BUFFERS:
      connection_parse_use_buffers (conn, command);
      break;

    case PINOS_CONTROL_CMD_PROCESS_BUFFER:
      if (conn->in.size < sizeof (PinosControlCmdProcessBuffer))
        return FALSE;
      memcpy (command, conn->in.data, sizeof (PinosControlCmdProcessBuffer));
      break;

    case PINOS_CONTROL_CMD_NODE_EVENT:
      connection_parse_node_event (conn, command);
      break;

    case PINOS_CONTROL_CMD_NODE_COMMAND:
      connection_parse_node_command (conn, command);
      break;

    case PINOS_CONTROL_CMD_INVALID:
      return FALSE;
  }
  return TRUE;
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
                         guint            index,
                         gboolean         close)
{
  int fd;

  g_return_val_if_fail (conn != NULL, -1);
  g_return_val_if_fail (index < conn->in.n_fds, -1);

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
                         gboolean         close)
{
  int index, i;

  g_return_val_if_fail (conn != NULL, -1);

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
 * pinos_connection_add_cmd:
 * @conn: a #PinosConnection
 * @cmd: a #PinosControlCmd
 * @command: a command
 *
 * Add a @cmd to @conn with data from @command.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_connection_add_cmd (PinosConnection *conn,
                          PinosControlCmd  cmd,
                          gpointer         command)
{
  void *p;

  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (command != NULL, FALSE);

  switch (cmd) {
    /* C -> S */
    case PINOS_CONTROL_CMD_NODE_UPDATE:
      connection_add_node_update (conn, command);
      break;

    case PINOS_CONTROL_CMD_PORT_UPDATE:
      connection_add_port_update (conn, command);
      break;

    case PINOS_CONTROL_CMD_PORT_STATUS_CHANGE:
      p = connection_add_cmd (conn, cmd, 0);
      break;

    case PINOS_CONTROL_CMD_NODE_STATE_CHANGE:
      p = connection_add_cmd (conn, cmd, sizeof (PinosControlCmdNodeStateChange));
      memcpy (p, command, sizeof (PinosControlCmdNodeStateChange));
      break;

    /* S -> C */
    case PINOS_CONTROL_CMD_ADD_PORT:
      p = connection_add_cmd (conn, cmd, sizeof (PinosControlCmdAddPort));
      memcpy (p, command, sizeof (PinosControlCmdAddPort));
      break;

    case PINOS_CONTROL_CMD_REMOVE_PORT:
      p = connection_add_cmd (conn, cmd, sizeof (PinosControlCmdRemovePort));
      memcpy (p, command, sizeof (PinosControlCmdRemovePort));
      break;

    case PINOS_CONTROL_CMD_SET_FORMAT:
      connection_add_set_format (conn, command);
      break;

    case PINOS_CONTROL_CMD_SET_PROPERTY:
      g_warning ("implement builder of %d", cmd);
      break;

    /* bidirectional */
    case PINOS_CONTROL_CMD_ADD_MEM:
      p = connection_add_cmd (conn, cmd, sizeof (PinosControlCmdAddMem));
      memcpy (p, command, sizeof (PinosControlCmdAddMem));
      break;

    case PINOS_CONTROL_CMD_USE_BUFFERS:
      connection_add_use_buffers (conn, command);
      break;

    case PINOS_CONTROL_CMD_PROCESS_BUFFER:
      p = connection_add_cmd (conn, cmd, sizeof (PinosControlCmdProcessBuffer));
      memcpy (p, command, sizeof (PinosControlCmdProcessBuffer));
      break;

    case PINOS_CONTROL_CMD_NODE_EVENT:
      connection_add_node_event (conn, command);
      break;

    case PINOS_CONTROL_CMD_NODE_COMMAND:
      connection_add_node_command (conn, command);
      break;

    case PINOS_CONTROL_CMD_INVALID:
      return FALSE;
  }
  return TRUE;
}

gboolean
pinos_connection_flush (PinosConnection *conn)
{
  ssize_t len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];
  int *cm, i, fds_len;
  ConnectionBuffer *buf;

  g_return_val_if_fail (conn != NULL, FALSE);

  buf = &conn->out;

  if (buf->buffer_size == 0)
    return TRUE;

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

  PINOS_DEBUG_CONTROL ("connection %p: %d written %zd bytes and %u fds", conn, conn->fd, len, buf->n_fds);

  return TRUE;

  /* ERRORS */
send_error:
  {
    g_warning ("could not sendmsg: %s", strerror (errno));
    return FALSE;
  }
}

gboolean
pinos_connection_clear (PinosConnection *conn)
{
  g_return_val_if_fail (conn != NULL, FALSE);

  clear_buffer (&conn->out);
  clear_buffer (&conn->in);
  conn->in.update = true;

  return TRUE;
}
