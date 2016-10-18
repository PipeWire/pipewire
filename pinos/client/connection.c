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

  SpaControlCmd   cmd;
  off_t           offset;
  void           *data;
  size_t          size;

  bool            update;
} ConnectionBuffer;

struct _SpaConnection {
  ConnectionBuffer in, out;
  int fd;
};

#if 0
#define SPA_DEBUG_CONTROL(format,args...) fprintf(stderr,format,##args)
#else
#define SPA_DEBUG_CONTROL(format,args...)
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
connection_parse_node_update (SpaConnection *conn, SpaControlCmdNodeUpdate *nu)
{
  memcpy (nu, conn->in.data, sizeof (SpaControlCmdNodeUpdate));
  if (nu->props)
    nu->props = spa_serialize_props_deserialize (conn->in.data, SPA_PTR_TO_INT (nu->props));
}

static void
connection_parse_port_update (SpaConnection *conn, SpaControlCmdPortUpdate *pu)
{
  void *p;
  unsigned int i;

  memcpy (pu, conn->in.data, sizeof (SpaControlCmdPortUpdate));

  p = conn->in.data;

  if (pu->possible_formats)
    pu->possible_formats = SPA_MEMBER (p,
                        SPA_PTR_TO_INT (pu->possible_formats), SpaFormat *);
  for (i = 0; i < pu->n_possible_formats; i++) {
    if (pu->possible_formats[i]) {
      pu->possible_formats[i] = spa_serialize_format_deserialize (p,
                          SPA_PTR_TO_INT (pu->possible_formats[i]));
    }
  }
  if (pu->format)
    pu->format = spa_serialize_format_deserialize (p, SPA_PTR_TO_INT (pu->format));
  if (pu->props)
    pu->props = spa_serialize_props_deserialize (p, SPA_PTR_TO_INT (pu->props));
  if (pu->info)
    pu->info = spa_serialize_port_info_deserialize (p, SPA_PTR_TO_INT (pu->info));
}

static void
connection_parse_set_format (SpaConnection *conn, SpaControlCmdSetFormat *cmd)
{
  memcpy (cmd, conn->in.data, sizeof (SpaControlCmdSetFormat));
  if (cmd->format)
    cmd->format = spa_serialize_format_deserialize (conn->in.data, SPA_PTR_TO_INT (cmd->format));
}

static void
connection_parse_use_buffers (SpaConnection *conn, SpaControlCmdUseBuffers *cmd)
{
  void *p;

  p = conn->in.data;
  memcpy (cmd, p, sizeof (SpaControlCmdUseBuffers));
  if (cmd->buffers)
    cmd->buffers = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->buffers), SpaControlMemRef);
}

static void
connection_parse_node_event (SpaConnection *conn, SpaControlCmdNodeEvent *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (SpaControlCmdNodeEvent));
  if (cmd->event)
    cmd->event = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->event), SpaNodeEvent);
  if (cmd->event->data)
    cmd->event->data = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->event->data), void);
}

static void
connection_parse_node_command (SpaConnection *conn, SpaControlCmdNodeCommand *cmd)
{
  void *p = conn->in.data;
  memcpy (cmd, p, sizeof (SpaControlCmdNodeCommand));
  if (cmd->command)
    cmd->command = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->command), SpaNodeCommand);
  if (cmd->command->data)
    cmd->command->data = SPA_MEMBER (p, SPA_PTR_TO_INT (cmd->command->data), void);
}

#define MAX(a,b)  ((a) > (b) ? (a) : (b))

static void *
connection_ensure_size (SpaConnection *conn, ConnectionBuffer *buf, size_t size)
{
  if (buf->buffer_size + size > buf->buffer_maxsize) {
    buf->buffer_maxsize = buf->buffer_size + MAX_BUFFER_SIZE * ((size + MAX_BUFFER_SIZE-1) / MAX_BUFFER_SIZE);
    buf->buffer_data = realloc (buf->buffer_data, buf->buffer_maxsize);
    fprintf (stderr, "connection %p: resize buffer to %zd\n", conn, buf->buffer_maxsize);
  }
  return (uint8_t *) buf->buffer_data + buf->buffer_size;
}

static void *
connection_add_cmd (SpaConnection *conn, SpaControlCmd cmd, size_t size)
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
connection_add_node_update (SpaConnection *conn, SpaControlCmdNodeUpdate *nu)
{
  size_t len;
  void *p;
  SpaControlCmdNodeUpdate *d;

  /* calc len */
  len = sizeof (SpaControlCmdNodeUpdate);
  len += spa_serialize_props_get_size (nu->props);

  p = connection_add_cmd (conn, SPA_CONTROL_CMD_NODE_UPDATE, len);
  memcpy (p, nu, sizeof (SpaControlCmdNodeUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (SpaControlCmdNodeUpdate), void);
  if (nu->props) {
    len = spa_serialize_props_serialize (p, nu->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  } else {
    d->props = 0;
  }
}

static void
connection_add_port_update (SpaConnection *conn, SpaControlCmdPortUpdate *pu)
{
  size_t len;
  void *p;
  int i;
  SpaFormat **bfa;
  SpaControlCmdPortUpdate *d;

  /* calc len */
  len = sizeof (SpaControlCmdPortUpdate);
  len += pu->n_possible_formats * sizeof (SpaFormat *);
  for (i = 0; i < pu->n_possible_formats; i++) {
    len += spa_serialize_format_get_size (pu->possible_formats[i]);
  }
  len += spa_serialize_format_get_size (pu->format);
  len += spa_serialize_props_get_size (pu->props);
  if (pu->info)
    len += spa_serialize_port_info_get_size (pu->info);

  p = connection_add_cmd (conn, SPA_CONTROL_CMD_PORT_UPDATE, len);
  memcpy (p, pu, sizeof (SpaControlCmdPortUpdate));
  d = p;

  p = SPA_MEMBER (d, sizeof (SpaControlCmdPortUpdate), void);
  bfa = p;
  if (pu->n_possible_formats)
    d->possible_formats = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  else
    d->possible_formats = 0;

  p = SPA_MEMBER (p, sizeof (SpaFormat*) * pu->n_possible_formats, void);

  for (i = 0; i < pu->n_possible_formats; i++) {
    len = spa_serialize_format_serialize (p, pu->possible_formats[i]);
    bfa[i] = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  }
  if (pu->format) {
    len = spa_serialize_format_serialize (p, pu->format);
    d->format = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->format = 0;
  }
  if (pu->props) {
    len = spa_serialize_props_serialize (p, pu->props);
    d->props = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->props = 0;
  }
  if (pu->info) {
    len = spa_serialize_port_info_serialize (p, pu->info);
    d->info = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
    p = SPA_MEMBER (p, len, void);
  } else {
    d->info = 0;
  }
}

static void
connection_add_set_format (SpaConnection *conn, SpaControlCmdSetFormat *sf)
{
  size_t len;
  void *p;

  /* calculate length */
  /* port_id + format + mask  */
  len = sizeof (SpaControlCmdSetFormat) + spa_serialize_format_get_size (sf->format);
  p = connection_add_cmd (conn, SPA_CONTROL_CMD_SET_FORMAT, len);
  memcpy (p, sf, sizeof (SpaControlCmdSetFormat));
  sf = p;

  p = SPA_MEMBER (sf, sizeof (SpaControlCmdSetFormat), void);
  if (sf->format) {
    len = spa_serialize_format_serialize (p, sf->format);
    sf->format = SPA_INT_TO_PTR (SPA_PTRDIFF (p, sf));
  } else
    sf->format = 0;
}

static void
connection_add_use_buffers (SpaConnection *conn, SpaControlCmdUseBuffers *ub)
{
  size_t len;
  int i;
  SpaControlCmdUseBuffers *d;
  SpaControlMemRef *mr;

  /* calculate length */
  len = sizeof (SpaControlCmdUseBuffers);
  len += ub->n_buffers * sizeof (SpaControlMemRef);

  d = connection_add_cmd (conn, SPA_CONTROL_CMD_USE_BUFFERS, len);
  memcpy (d, ub, sizeof (SpaControlCmdUseBuffers));

  mr = SPA_MEMBER (d, sizeof (SpaControlCmdUseBuffers), void);

  if (d->n_buffers)
    d->buffers = SPA_INT_TO_PTR (SPA_PTRDIFF (mr, d));
  else
    d->buffers = 0;

  for (i = 0; i < ub->n_buffers; i++)
    memcpy (&mr[i], &ub->buffers[i], sizeof (SpaControlMemRef));
}

static void
connection_add_node_event (SpaConnection *conn, SpaControlCmdNodeEvent *ev)
{
  size_t len;
  void *p;
  SpaControlCmdNodeEvent *d;
  SpaNodeEvent *ne;

  /* calculate length */
  len = sizeof (SpaControlCmdNodeEvent);
  len += sizeof (SpaNodeEvent);
  len += ev->event->size;

  p = connection_add_cmd (conn, SPA_CONTROL_CMD_NODE_EVENT, len);
  memcpy (p, ev, sizeof (SpaControlCmdNodeEvent));
  d = p;

  p = SPA_MEMBER (d, sizeof (SpaControlCmdNodeEvent), void);
  d->event = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  ne = p;
  memcpy (p, ev->event, sizeof (SpaNodeEvent));
  p = SPA_MEMBER (p, sizeof (SpaNodeEvent), void);
  ne->data = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  memcpy (p, ev->event->data, ev->event->size);
}


static void
connection_add_node_command (SpaConnection *conn, SpaControlCmdNodeCommand *cm)
{
  size_t len;
  void *p;
  SpaControlCmdNodeCommand *d;
  SpaNodeCommand *nc;

  /* calculate length */
  len = sizeof (SpaControlCmdNodeCommand);
  len += sizeof (SpaNodeCommand);
  len += cm->command->size;

  p = connection_add_cmd (conn, SPA_CONTROL_CMD_NODE_COMMAND, len);
  memcpy (p, cm, sizeof (SpaControlCmdNodeCommand));
  d = p;

  p = SPA_MEMBER (d, sizeof (SpaControlCmdNodeCommand), void);
  d->command = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));

  nc = p;
  memcpy (p, cm->command, sizeof (SpaNodeCommand));
  p = SPA_MEMBER (p, sizeof (SpaNodeCommand), void);
  nc->data = SPA_INT_TO_PTR (SPA_PTRDIFF (p, d));
  memcpy (p, cm->command->data, cm->command->size);
}

static SpaResult
refill_buffer (SpaConnection *conn, ConnectionBuffer *buf)
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
    return SPA_RESULT_ERROR;

  buf->buffer_size += len;

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    buf->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (buf->fds, CMSG_DATA (cmsg), buf->n_fds * sizeof (int));
  }
  SPA_DEBUG_CONTROL ("connection %p: %d read %zd bytes and %d fds\n", conn, conn->fd, len, buf->n_fds);

  return SPA_RESULT_OK;

  /* ERRORS */
recv_error:
  {
    fprintf (stderr, "could not recvmsg on fd %d: %s\n", conn->fd, strerror (errno));
    return SPA_RESULT_ERROR;
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
  buf->cmd = SPA_CONTROL_CMD_INVALID;
  buf->offset = 0;
  buf->size = 0;
  buf->buffer_size = 0;
}

SpaConnection *
spa_connection_new (int fd)
{
  SpaConnection *c;

  c = calloc (1, sizeof (SpaConnection));
  c->fd = fd;
  c->out.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->out.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->in.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.update = true;

  return c;
}

void
spa_connection_free (SpaConnection *conn)
{
  free (conn->out.buffer_data);
  free (conn->in.buffer_data);
  free (conn);
}

/**
 * spa_connection_has_next:
 * @iter: a #SpaConnection
 *
 * Move to the next packet in @conn.
 *
 * Returns: %SPA_RESULT_OK if more packets are available.
 */
SpaResult
spa_connection_has_next (SpaConnection *conn)
{
  size_t len, size, skip;
  uint8_t *data;
  ConnectionBuffer *buf;

  if (conn == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
    return SPA_RESULT_ERROR;
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

  return SPA_RESULT_OK;
}

SpaControlCmd
spa_connection_get_cmd (SpaConnection *conn)
{
  if (conn == NULL)
    return SPA_CONTROL_CMD_INVALID;

  return conn->in.cmd;
}

SpaResult
spa_connection_parse_cmd (SpaConnection *conn,
                          void          *command)
{
  SpaResult res = SPA_RESULT_OK;

  if (conn == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (conn->in.cmd) {
    /* C -> S */
    case SPA_CONTROL_CMD_NODE_UPDATE:
      connection_parse_node_update (conn, command);
      break;

    case SPA_CONTROL_CMD_PORT_UPDATE:
      connection_parse_port_update (conn, command);
      break;

    case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      fprintf (stderr, "implement iter of %d\n", conn->in.cmd);
      break;

    case SPA_CONTROL_CMD_NODE_STATE_CHANGE:
      if (conn->in.size < sizeof (SpaControlCmdNodeStateChange))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdNodeStateChange));
      break;

    /* S -> C */
    case SPA_CONTROL_CMD_ADD_PORT:
      if (conn->in.size < sizeof (SpaControlCmdAddPort))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdAddPort));
      break;

    case SPA_CONTROL_CMD_REMOVE_PORT:
      if (conn->in.size < sizeof (SpaControlCmdRemovePort))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdRemovePort));
      break;

    case SPA_CONTROL_CMD_SET_FORMAT:
      connection_parse_set_format (conn, command);
      break;

    case SPA_CONTROL_CMD_SET_PROPERTY:
      fprintf (stderr, "implement iter of %d\n", conn->in.cmd);
      break;

    /* bidirectional */
    case SPA_CONTROL_CMD_ADD_MEM:
      if (conn->in.size < sizeof (SpaControlCmdAddMem))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdAddMem));
      break;

    case SPA_CONTROL_CMD_REMOVE_MEM:
      if (conn->in.size < sizeof (SpaControlCmdRemoveMem))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdRemoveMem));
      break;

    case SPA_CONTROL_CMD_USE_BUFFERS:
      connection_parse_use_buffers (conn, command);
      break;

    case SPA_CONTROL_CMD_PROCESS_BUFFER:
      if (conn->in.size < sizeof (SpaControlCmdProcessBuffer))
        return SPA_RESULT_ERROR;
      memcpy (command, conn->in.data, sizeof (SpaControlCmdProcessBuffer));
      break;

    case SPA_CONTROL_CMD_NODE_EVENT:
      connection_parse_node_event (conn, command);
      break;

    case SPA_CONTROL_CMD_NODE_COMMAND:
      connection_parse_node_command (conn, command);
      break;

    case SPA_CONTROL_CMD_INVALID:
      return SPA_RESULT_ERROR;
  }
  return res;
}

/**
 * spa_connection_get_fd:
 * @conn: a #SpaConnection
 * @index: an index
 * @steal: steal the fd
 *
 * Get the file descriptor at @index in @conn.
 *
 * Returns: a file descriptor at @index in @conn. The file descriptor
 * is not duplicated in any way. -1 is returned on error.
 */
int
spa_connection_get_fd (SpaConnection *conn,
                       unsigned int   index,
                       bool           close)
{
  int fd;

  if (conn == NULL || conn->in.n_fds < index)
    return -1;

  fd = conn->in.fds[index];
  if (fd < 0)
    fd = -fd;
  conn->in.fds[index] = close ? fd : -fd;

  return fd;
}

/**
 * spa_connection_add_fd:
 * @conn: a #SpaConnection
 * @fd: a valid fd
 * @close: if the descriptor should be closed when sent
 *
 * Add the file descriptor @fd to @builder.
 *
 * Returns: the index of the file descriptor in @builder.
 */
int
spa_connection_add_fd (SpaConnection *conn,
                       int            fd,
                       bool           close)
{
  int index, i;

  if (conn == NULL)
    return -1;

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
 * spa_connection_add_cmd:
 * @conn: a #SpaConnection
 * @cmd: a #SpaControlCmd
 * @command: a command
 *
 * Add a @cmd to @conn with data from @command.
 *
 * Returns: %SPA_RESULT_OK on success.
 */
SpaResult
spa_connection_add_cmd (SpaConnection *conn,
                        SpaControlCmd  cmd,
                        void          *command)
{
  void *p;

  if (conn == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (cmd) {
    /* C -> S */
    case SPA_CONTROL_CMD_NODE_UPDATE:
      connection_add_node_update (conn, command);
      break;

    case SPA_CONTROL_CMD_PORT_UPDATE:
      connection_add_port_update (conn, command);
      break;

    case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      p = connection_add_cmd (conn, cmd, 0);
      break;

    case SPA_CONTROL_CMD_NODE_STATE_CHANGE:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdNodeStateChange));
      memcpy (p, command, sizeof (SpaControlCmdNodeStateChange));
      break;

    /* S -> C */
    case SPA_CONTROL_CMD_ADD_PORT:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdAddPort));
      memcpy (p, command, sizeof (SpaControlCmdAddPort));
      break;

    case SPA_CONTROL_CMD_REMOVE_PORT:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdRemovePort));
      memcpy (p, command, sizeof (SpaControlCmdRemovePort));
      break;

    case SPA_CONTROL_CMD_SET_FORMAT:
      connection_add_set_format (conn, command);
      break;

    case SPA_CONTROL_CMD_SET_PROPERTY:
      fprintf (stderr, "implement builder of %d\n", cmd);
      break;

    /* bidirectional */
    case SPA_CONTROL_CMD_ADD_MEM:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdAddMem));
      memcpy (p, command, sizeof (SpaControlCmdAddMem));
      break;

    case SPA_CONTROL_CMD_REMOVE_MEM:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdRemoveMem));
      memcpy (p, command, sizeof (SpaControlCmdRemoveMem));
      break;

    case SPA_CONTROL_CMD_USE_BUFFERS:
      connection_add_use_buffers (conn, command);
      break;

    case SPA_CONTROL_CMD_PROCESS_BUFFER:
      p = connection_add_cmd (conn, cmd, sizeof (SpaControlCmdProcessBuffer));
      memcpy (p, command, sizeof (SpaControlCmdProcessBuffer));
      break;

    case SPA_CONTROL_CMD_NODE_EVENT:
      connection_add_node_event (conn, command);
      break;

    case SPA_CONTROL_CMD_NODE_COMMAND:
      connection_add_node_command (conn, command);
      break;

    case SPA_CONTROL_CMD_INVALID:
      return SPA_RESULT_INVALID_ARGUMENTS;
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_connection_flush (SpaConnection *conn)
{
  ssize_t len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (MAX_FDS * sizeof (int))];
  int *cm, i, fds_len;
  ConnectionBuffer *buf;

  if (conn == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  buf = &conn->out;

  if (buf->buffer_size == 0)
    return SPA_RESULT_OK;

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

  SPA_DEBUG_CONTROL ("connection %p: %d written %zd bytes and %u fds\n", conn, conn->fd, len, buf->n_fds);

  return SPA_RESULT_OK;

  /* ERRORS */
send_error:
  {
    fprintf (stderr, "could not sendmsg: %s\n", strerror (errno));
    return SPA_RESULT_ERROR;
  }
}

SpaResult
spa_connection_clear (SpaConnection *conn)
{
  if (conn == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  clear_buffer (&conn->out);
  clear_buffer (&conn->in);
  conn->in.update = true;

  return SPA_RESULT_OK;
}
