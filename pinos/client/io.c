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

#include <sys/socket.h>
#include <string.h>
#include <errno.h>

#include <gio/gio.h>

#include "pinos/client/pinos.h"
#include "pinos/client/private.h"

gboolean
pinos_io_read_buffer (int           fd,
                      PinosBuffer  *buffer,
                      void         *data,
                      size_t        max_data,
                      int          *fds,
                      size_t        max_fds,
                      GError      **error)
{
  gssize len;
  PinosStackHeader *hdr;
  PinosStackBuffer *sb = (PinosStackBuffer *) buffer;
  gsize need;
  struct cmsghdr *cmsg;
  struct msghdr msg = {0};
  struct iovec iov[1];
  char cmsgbuf[CMSG_SPACE (max_fds * sizeof (int))];

  g_assert (sb->refcount == 0);

  sb->data = data;
  sb->max_size = max_data;
  sb->size = 0;
  sb->free_data = NULL;
  sb->fds = fds;
  sb->max_fds = max_fds;
  sb->n_fds = 0;
  sb->free_fds = NULL;

  hdr = sb->data;

  /* read header and control messages first */
  iov[0].iov_base = hdr;
  iov[0].iov_len = sizeof (PinosStackHeader);;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);
  msg.msg_flags = MSG_CMSG_CLOEXEC;

  while (TRUE) {
    len = recvmsg (fd, &msg, msg.msg_flags);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto recv_error;
    }
    break;
  }
  g_assert (len == sizeof (PinosStackHeader));

  /* now we know the total length */
  need = sizeof (PinosStackHeader) + hdr->length;

  if (sb->max_size < need) {
    g_warning ("io: realloc receive memory %" G_GSIZE_FORMAT" -> %" G_GSIZE_FORMAT, sb->max_size, need);
    sb->max_size = need;
    hdr = sb->data = sb->free_data = g_realloc (sb->free_data, need);
  }
  sb->size = need;

  if (hdr->length > 0) {
    /* read data */
    while (TRUE) {
      len = recv (fd, (gchar *)sb->data + sizeof (PinosStackHeader), hdr->length, 0);
      if (len < 0) {
        if (errno == EINTR)
          continue;
        else
          goto recv_error;
      }
      break;
    }
    g_assert (len == hdr->length);
  }

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    sb->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (sb->fds, CMSG_DATA (cmsg), sb->n_fds * sizeof (int));
  }
  sb->refcount = 1;
  sb->magic = PSB_MAGIC;

  return TRUE;

  /* ERRORS */
recv_error:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not recvmsg: %s", strerror (errno));
    return FALSE;
  }
}

gboolean
pinos_io_write_buffer (int          fd,
                       PinosBuffer *buffer,
                       GError     **error)
{
  PinosStackBuffer *sb = (PinosStackBuffer *) buffer;
  gssize len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (sb->n_fds * sizeof (int))];
  gint fds_len = sb->n_fds * sizeof (int);

  iov[0].iov_base = sb->data;
  iov[0].iov_len = sb->size;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = CMSG_SPACE (fds_len);
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (fds_len);
  memcpy(CMSG_DATA(cmsg), sb->fds, fds_len);
  msg.msg_controllen = cmsg->cmsg_len;

  while (TRUE) {
    len = sendmsg (fd, &msg, 0);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto send_error;
    }
    break;
  }
  g_assert (len == (gssize) sb->size);

  return TRUE;

  /* ERRORS */
send_error:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not sendmsg: %s", strerror (errno));
    return FALSE;
  }
}
