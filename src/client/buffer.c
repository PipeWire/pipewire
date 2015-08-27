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

#include <gio/gio.h>
#include <gio/gunixfdmessage.h>

#include "client/properties.h"
#include "client/context.h"
#include "client/buffer.h"
#include "client/private.h"

G_STATIC_ASSERT (sizeof (PinosStackBuffer) <= sizeof (PinosBuffer));

/**
 * pinos_buffer_init_data:
 * @buffer: a #PinosBuffer
 * @data: data
 * @size: size of @data
 * @message: (transfer full): a #GSocketControlMessage
 *
 * Initialize @buffer with @data and @size and @message. @data and size
 * must not be modified until pinos_buffer_clear() is called.
 *
 * Ownership is taken of @message.
 */
void
pinos_buffer_init_data (PinosBuffer       *buffer,
                        gpointer           data,
                        gsize              size,
                        GSocketControlMessage *message)
{
  PinosStackBuffer *sb = PSB (buffer);

  sb->magic = PSB_MAGIC;
  sb->data = data;
  sb->size = size;
  sb->allocated_size = 0;
  sb->message = message;
}

void
pinos_buffer_clear (PinosBuffer *buffer)
{
  PinosStackBuffer *sb = PSB (buffer);

  g_return_val_if_fail (is_valid_buffer (buffer), -1);

  sb->magic = 0;
  if (sb->allocated_size)
    g_free (sb->data);
  sb->size = 0;
  sb->allocated_size = 0;
  g_clear_object (&sb->message);
}

const PinosBufferHeader *
pinos_buffer_get_header (PinosBuffer *buffer, guint32 *version)
{
  PinosStackBuffer *sb = PSB (buffer);
  PinosStackHeader *hdr;

  g_return_val_if_fail (is_valid_buffer (buffer), NULL);

  hdr = sb->data;

  if (version)
    *version = hdr->version;

  return (const PinosBufferHeader *) &hdr->header;
}

/**
 * pinos_buffer_get_fd:
 * @buffer: a #PinosBuffer
 * @index: an index
 * @error: a #GError or %NULL
 *
 * Get the file descriptor at @index in @buffer.
 *
 * Returns: a file descriptor ar @index in @buffer. The file descriptor is
 * duplicated using dup() and set as close-on-exec before being returned.
 * You must call close() on it when you are done. -1 is returned on error and
 * @error is set.
 */
int
pinos_buffer_get_fd (PinosBuffer *buffer, gint index, GError **error)
{
  PinosStackBuffer *sb = PSB (buffer);
  GUnixFDList *fds;

  g_return_val_if_fail (is_valid_buffer (buffer), -1);
  g_return_val_if_fail (sb->message != NULL, -1);

  if (g_socket_control_message_get_msg_type (sb->message) != SCM_RIGHTS)
    goto not_found;

  fds = g_unix_fd_message_get_fd_list (G_UNIX_FD_MESSAGE (sb->message));
  if (fds == NULL)
    goto not_found;

  if (g_unix_fd_list_get_length (fds) <= index)
    goto not_found;

  return g_unix_fd_list_get (fds, index, error);

  /* ERRORS */
not_found:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Buffer does not have any fd at index %d", index);
    return -1;
  }
}

/**
 * pinos_buffer_steal_data:
 * @buffer: a #PinosBuffer
 * @size: output size
 * @message: output #GSocketControlMessage
 *
 * Take the data and control message from @buffer.
 *
 * Returns: the data of @buffer.
 */
gpointer
pinos_buffer_steal (PinosBuffer       *buffer,
                    gsize             *size,
                    GSocketControlMessage **message)
{
  PinosStackBuffer *sb = PSB (buffer);
  gpointer data;

  g_return_val_if_fail (is_valid_buffer (buffer), 0);

  if (size)
    *size = sb->size;
  if (message)
    *message = sb->message;

  data = sb->data;

  sb->magic = 0;
  sb->data = NULL;
  sb->size = 0;
  sb->allocated_size = 0;
  sb->message = NULL;

  return data;
}

/**
 * PinosBufferIter:
 *
 * #PinosBufferIter is an opaque data structure and can only be accessed
 * using the following functions.
 */
struct stack_iter {
  gsize             magic;
  guint32           version;
  PinosStackBuffer *buffer;
  gsize             offset;

  PinosPacketType   type;
  gsize             size;
  gpointer          data;

  guint             item;
};

G_STATIC_ASSERT (sizeof (struct stack_iter) <= sizeof (PinosBufferIter));

#define PPSI(i)             ((struct stack_iter *) (i))
#define PPSI_MAGIC          ((gsize) 6739527471u)
#define is_valid_iter(i)    (i != NULL && \
                             PPSI(i)->magic == PPSI_MAGIC)

/**
 * pinos_buffer_iter_init:
 * @iter: a #PinosBufferIter
 * @buffer: a #PinosBuffer
 *
 * Initialize @iter to iterate the packets in @buffer.
 */
void
pinos_buffer_iter_init_full (PinosBufferIter *iter,
                             PinosBuffer     *buffer,
                             guint32          version)
{
  struct stack_iter *si = PPSI (iter);

  g_return_if_fail (iter != NULL);
  g_return_if_fail (is_valid_buffer (buffer));

  si->magic = PPSI_MAGIC;
  si->version = version;
  si->buffer = PSB (buffer);
  si->offset = 0;
  si->type = PINOS_PACKET_TYPE_INVALID;
  si->size = sizeof (PinosStackHeader);
  si->data = NULL;
  si->item = 0;
}

static gboolean
read_length (guint8 * data, guint size, gsize * length, gsize * skip)
{
  gsize len, offset;
  guint8 b;

  /* start reading the length, we need this to skip to the data later */
  len = offset = 0;
  do {
    if (offset >= size)
      return FALSE;
    b = data[offset++];
    len = (len << 7) | (b & 0x7f);
  } while (b & 0x80);

  /* check remaining buffer size */
  if (size - offset < len)
    return FALSE;

  *length = len;
  *skip = offset;

  return TRUE;
}


/**
 * pinos_buffer_iter_next:
 * @iter: a #PinosBufferIter
 *
 * Move to the next packet in @iter.
 *
 * Returns: %TRUE if more packets are available.
 */
gboolean
pinos_buffer_iter_next (PinosBufferIter *iter)
{
  struct stack_iter *si = PPSI (iter);
  gsize len, size, skip;
  guint8 *data;

  g_return_val_if_fail (is_valid_iter (iter), FALSE);

  /* move to next packet */
  si->offset += si->size;

  /* now read packet */
  data = si->buffer->data;
  size = si->buffer->size;
  if (si->offset >= size)
    return FALSE;

  data += si->offset;
  size -= si->offset;

  if (size < 1)
    return FALSE;

  si->type = *data;

  data++;
  size--;

  if (!read_length (data, size, &len, &skip))
    return FALSE;

  si->size = len;
  si->data = data + skip;
  si->offset += 1 + skip;

  return TRUE;
}

PinosPacketType
pinos_buffer_iter_get_type (PinosBufferIter *iter)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), PINOS_PACKET_TYPE_INVALID);

  return si->type;
}

gpointer
pinos_buffer_iter_get_data (PinosBufferIter *iter, gsize *size)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), NULL);

  if (size)
    *size = si->size;

  return si->data;
}


/**
 * PinosBufferBuilder:
 * @buffer: owner #PinosBuffer
 */
struct stack_builder {
  gsize             magic;

  PinosStackHeader *sh;
  PinosStackBuffer  buf;

  PinosPacketType   type;
  gsize             offset;

  guint             n_fds;
};

G_STATIC_ASSERT (sizeof (struct stack_builder) <= sizeof (PinosBufferBuilder));

#define PPSB(b)             ((struct stack_builder *) (b))
#define PPSB_MAGIC          ((gsize) 8103647428u)
#define is_valid_builder(b) (b != NULL && \
                             PPSB(b)->magic == PPSB_MAGIC)


/**
 * pinos_buffer_builder_init_full:
 * @builder: a #PinosBufferBuilder
 * @version: a version
 *
 * Initialize a stack allocated @builder and set the @version.
 */
void
pinos_buffer_builder_init_full (PinosBufferBuilder       *builder,
                                guint32                   version)
{
  struct stack_builder *sb = PPSB (builder);
  PinosStackHeader *sh;

  g_return_if_fail (builder != NULL);

  sb->magic = PPSB_MAGIC;
  sb->buf.allocated_size = sizeof (PinosStackHeader) + 128;
  sb->buf.data = g_malloc (sb->buf.allocated_size);
  sb->buf.size = sizeof (PinosStackHeader);
  sb->buf.message = NULL;

  sh = sb->sh = sb->buf.data;
  sh->version = version;
  sh->length = 0;

  sb->type = 0;
  sb->offset = 0;
  sb->n_fds = 0;
}

/**
 * pinos_buffer_builder_clear:
 * @builder: a #PinosBufferBuilder
 *
 * Clear the memory used by @builder. This can be used to abort building the
 * buffer.
 *
 * @builder becomes invalid after this function and can be reused with
 * pinos_buffer_builder_init()
 */
void
pinos_buffer_builder_clear (PinosBufferBuilder *builder)
{
  struct stack_builder *sb = PPSB (builder);

  g_return_if_fail (is_valid_builder (builder));

  sb->magic = 0;
  g_free (sb->buf.data);
}

/**
 * pinos_buffer_builder_end:
 * @builder: a #PinosBufferBuilder
 * @buffer: a #PinosBuffer
 *
 * Ends the building process and fills @buffer with the constructed
 * #PinosBuffer.
 *
 * @builder becomes invalid after this function and can be reused with
 * pinos_buffer_builder_init()
 */
void
pinos_buffer_builder_end (PinosBufferBuilder *builder,
                          PinosBuffer        *buffer)
{
  struct stack_builder *sb = PPSB (builder);
  PinosStackBuffer *sbuf = PSB (buffer);

  g_return_if_fail (is_valid_builder (builder));
  g_return_if_fail (buffer != NULL);

  sb->sh->length = sb->buf.size - sizeof (PinosStackHeader);

  sbuf->magic = PSB_MAGIC;
  sbuf->data = sb->buf.data;
  sbuf->size = sb->buf.size;
  sbuf->allocated_size = sb->buf.allocated_size;
  sbuf->message = sb->buf.message;

  sb->buf.data = NULL;
  sb->buf.size = 0;
  sb->buf.allocated_size = 0;
  sb->buf.message = NULL;
  sb->magic = 0;
}

/**
 * pinos_buffer_builder_set_header:
 * @builder: a #PinosBufferBuilder
 * @header: a #PinosBufferHeader
 *
 * Set @header in @builder.
 */
void
pinos_buffer_builder_set_header (PinosBufferBuilder       *builder,
                                 const PinosBufferHeader  *header)
{
  struct stack_builder *sb = PPSB (builder);

  g_return_if_fail (is_valid_builder (builder));
  g_return_if_fail (header != NULL);

  sb->sh->header = *header;
}

/**
 * pinos_buffer_builder_add_fd:
 * @builder: a #PinosBufferBuilder
 * @fd: a valid fd
 * @error: a #GError or %NULL
 *
 * Add the file descriptor @fd to @builder.
 *
 * Returns: the index of the file descriptor in @builder. The file descriptor
 * is duplicated using dup(). You keep your copy of the descriptor and the copy
 * contained in @buffer will be closed when @buffer is freed.
 * -1 is returned on error and @error is set.
 */
gint
pinos_buffer_builder_add_fd (PinosBufferBuilder *builder,
                             int                 fd,
                             GError            **error)
{
  struct stack_builder *sb = PPSB (builder);

  g_return_val_if_fail (is_valid_builder (builder), -1);
  g_return_val_if_fail (fd > 0, -1);

  if (sb->buf.message == NULL)
    sb->buf.message = g_unix_fd_message_new ();

  if (!g_unix_fd_message_append_fd ((GUnixFDMessage*)sb->buf.message, fd, error))
    return -1;

  return sb->n_fds++;
}

static gpointer
builder_ensure_size (struct stack_builder *sb, gsize size)
{
  if (sb->buf.size + size > sb->buf.allocated_size) {
    sb->buf.allocated_size = sb->buf.size + MAX (size, 1024);
    sb->buf.data = g_realloc (sb->buf.data, sb->buf.allocated_size);
  }
  return (guint8 *) sb->buf.data + sb->buf.size;
}

static gpointer
builder_add_packet (struct stack_builder *sb, PinosPacketType type, gsize size)
{
  guint8 *p;
  guint plen;

  plen = 1;
  while (size >> (7 * plen))
    plen++;

  /* 1 for type, plen for size and size for payload */
  p = builder_ensure_size (sb, 1 + plen + size);

  sb->type = type;
  sb->offset = sb->buf.size;
  sb->buf.size += 1 + plen + size;

  *p++ = type;
  /* write length */
  while (plen) {
    plen--;
    *p++ = ((plen > 0) ? 0x80 : 0) | ((size >> (7 * plen)) & 0x7f);
  }
  return p;
}

/* fd-payload packets */
/**
 * pinos_buffer_iter_get_fd_payload:
 * @iter: a #PinosBufferIter
 * @payload: a #PinosPacketFDPayload
 *
 * Get the #PinosPacketFDPayload. @iter must be positioned on a packet of
 * type #PINOS_PACKET_TYPE_FD_PAYLOAD
 */
void
pinos_buffer_iter_parse_fd_payload (PinosBufferIter *iter,
                                    PinosPacketFDPayload *payload)
{
  struct stack_iter *si = PPSI (iter);

  g_return_if_fail (is_valid_iter (iter));
  g_return_if_fail (si->type == PINOS_PACKET_TYPE_FD_PAYLOAD);

  *payload = *((PinosPacketFDPayload *) si->data);
}

/**
 * pinos_buffer_builder_add_fd_payload:
 * @builder: a #PinosBufferBuilder
 * @payload: a #PinosPacketFDPayload
 *
 * Add a #PINOS_PACKET_TYPE_FD_PAYLOAD to @builder with data from @payload.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_buffer_builder_add_fd_payload (PinosBufferBuilder *builder,
                                     PinosPacketFDPayload *payload)
{
  struct stack_builder *sb = PPSB (builder);
  PinosPacketFDPayload *p;

  g_return_val_if_fail (is_valid_builder (builder), FALSE);
  g_return_val_if_fail (payload->size > 0, FALSE);

  p = builder_add_packet (sb, PINOS_PACKET_TYPE_FD_PAYLOAD, sizeof (PinosPacketFDPayload));
  *p = *payload;

  return TRUE;
}

