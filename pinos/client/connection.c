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
#include "log.h"

#define MAX_BUFFER_SIZE 1024
#define MAX_FDS 28

typedef struct {
  uint8_t         *buffer_data;
  size_t           buffer_size;
  size_t           buffer_maxsize;
  int              fds[MAX_FDS];
  uint32_t         n_fds;

  off_t            offset;
  void            *data;
  size_t           size;

  bool             update;
} ConnectionBuffer;

struct _PinosConnection {
  ConnectionBuffer in, out;
  int fd;
};

int
pinos_connection_get_fd (PinosConnection *conn,
                         uint32_t         index)
{
  if (index < 0 || index >= conn->in.n_fds)
    return -1;

  return conn->in.fds[index];
}

uint32_t
pinos_connection_add_fd (PinosConnection *conn,
                         int              fd)
{
  uint32_t index, i;

  for (i = 0; i < conn->out.n_fds; i++) {
    if (conn->out.fds[i] == fd)
      return i;
  }

  index = conn->out.n_fds;
  conn->out.fds[index] = fd;
  conn->out.n_fds++;

  return index;
}

static void *
connection_ensure_size (PinosConnection *conn, ConnectionBuffer *buf, size_t size)
{
  if (buf->buffer_size + size > buf->buffer_maxsize) {
    buf->buffer_maxsize = SPA_ROUND_UP_N (buf->buffer_size + size, MAX_BUFFER_SIZE);
    pinos_log_warn ("connection %p: resize buffer to %zd %zd %zd", conn, buf->buffer_size, size, buf->buffer_maxsize);
    buf->buffer_data = realloc (buf->buffer_data, buf->buffer_maxsize);
  }
  return (uint8_t *) buf->buffer_data + buf->buffer_size;
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

  buf->buffer_size += len;

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    buf->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (buf->fds, CMSG_DATA (cmsg), buf->n_fds * sizeof (int));
  }
  pinos_log_trace ("connection %p: %d read %zd bytes and %d fds", conn, conn->fd, len, buf->n_fds);

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
  buf->offset = 0;
  buf->size = 0;
  buf->buffer_size = 0;
}

PinosConnection *
pinos_connection_new (int fd)
{
  PinosConnection *c;

  c = calloc (1, sizeof (PinosConnection));
  if (c == NULL)
    return NULL;

  pinos_log_debug ("connection %p: new", c);

  c->fd = fd;
  c->out.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->out.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.buffer_data = malloc (MAX_BUFFER_SIZE);
  c->in.buffer_maxsize = MAX_BUFFER_SIZE;
  c->in.update = true;

  if (c->out.buffer_data == NULL || c->in.buffer_data == NULL)
    goto no_mem;

  return c;

no_mem:
  free (c->out.buffer_data);
  free (c->in.buffer_data);
  free (c);
  return NULL;
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
                           uint8_t          *opcode,
                           uint32_t         *dest_id,
                           void            **dt,
                           uint32_t         *sz)
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

  *dest_id = p[0];
  *opcode = p[1] >> 24;
  len = p[1] & 0xffffff;

  if (len > size) {
    connection_ensure_size (conn, buf, len);
    buf->update = true;
    goto again;
  }
  buf->size = len;
  buf->data = data;
  buf->offset += 8;

  *dt = buf->data;
  *sz = buf->size;

  return true;
}

void *
pinos_connection_begin_write (PinosConnection  *conn,
                              uint32_t          size)
{
  uint32_t *p;
  ConnectionBuffer *buf = &conn->out;
  /* 4 for dest_id, 1 for opcode, 3 for size and size for payload */
  p = connection_ensure_size (conn, buf, 8 + size);
  return p + 2;
}

void
pinos_connection_end_write (PinosConnection  *conn,
                            uint32_t          dest_id,
                            uint8_t           opcode,
                            uint32_t          size)
{
  uint32_t *p;
  ConnectionBuffer *buf = &conn->out;

  p = connection_ensure_size (conn, buf, 8 + size);
  *p++ = dest_id;
  *p++ = (opcode << 24) | (size & 0xffffff);

  buf->buffer_size += 8 + size;
  pinos_connection_flush (conn);
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
  pinos_log_trace ("connection %p: %d written %zd bytes and %u fds", conn, conn->fd, len, buf->n_fds);

  buf->buffer_size -= len;
  buf->n_fds = 0;

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
