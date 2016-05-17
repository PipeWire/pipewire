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

#include "pinos/client/properties.h"
#include "pinos/client/context.h"
#include "pinos/client/buffer.h"
#include "pinos/client/private.h"

G_STATIC_ASSERT (sizeof (PinosStackBuffer) <= sizeof (PinosBuffer));

/**
 * pinos_buffer_init_data:
 * @buffer: a #PinosBuffer
 * @data: data
 * @size: size of @data
 * @fds: file descriptors
 * @n_fds: number of file descriptors
 *
 * Initialize @buffer with @data and @size and @fds and @n_fds.
 * The memory pointer to by @data and @fds becomes property of @buffer
 * and should not be freed or modified until pinos_buffer_clear() is
 * called.
 */
void
pinos_buffer_init_data (PinosBuffer       *buffer,
                        gpointer           data,
                        gsize              size,
                        gint              *fds,
                        gint               n_fds)
{
  PinosStackBuffer *sb = PSB (buffer);

  sb->magic = PSB_MAGIC;
  sb->data = data;
  sb->size = size;
  sb->max_size = size;
  sb->free_data = NULL;
  sb->fds = fds;
  sb->n_fds = n_fds;
  sb->max_fds = n_fds;
  sb->free_fds = NULL;
}

void
pinos_buffer_clear (PinosBuffer *buffer)
{
  PinosStackBuffer *sb = PSB (buffer);
  gint i;

  g_return_if_fail (is_valid_buffer (buffer));

  sb->magic = 0;
  g_free (sb->free_data);
  for (i = 0; i < sb->n_fds; i++)
    close (sb->fds[i]);
  g_free (sb->free_fds);
  sb->n_fds = 0;
}

/**
 * pinos_buffer_get_version
 * @buffer: a #PinosBuffer
 *
 * Get the buffer version
 *
 * Returns: the buffer version.
 */
guint32
pinos_buffer_get_version (PinosBuffer *buffer)
{
  PinosStackBuffer *sb = PSB (buffer);
  PinosStackHeader *hdr;

  g_return_val_if_fail (is_valid_buffer (buffer), -1);

  hdr = sb->data;

  return hdr->version;
}

/**
 * pinos_buffer_get_flags
 * @buffer: a #PinosBuffer
 *
 * Get the buffer flags
 *
 * Returns: the buffer flags.
 */
PinosBufferFlags
pinos_buffer_get_flags (PinosBuffer *buffer)
{
  PinosStackBuffer *sb = PSB (buffer);
  PinosStackHeader *hdr;

  g_return_val_if_fail (is_valid_buffer (buffer), -1);

  hdr = sb->data;

  return hdr->flags;
}

/**
 * pinos_buffer_get_fd:
 * @buffer: a #PinosBuffer
 * @index: an index
 *
 * Get the file descriptor at @index in @buffer.
 *
 * Returns: a file descriptor at @index in @buffer. The file descriptor
 * is not duplicated in any way. -1 is returned on error.
 */
int
pinos_buffer_get_fd (PinosBuffer *buffer, gint index)
{
  PinosStackBuffer *sb = PSB (buffer);

  g_return_val_if_fail (is_valid_buffer (buffer), -1);

  if (sb->fds == NULL || sb->n_fds < index)
    return -1;

  return sb->fds[index];
}

/**
 * pinos_buffer_steal_data:
 * @buffer: a #PinosBuffer
 * @size: output size or %NULL to ignore
 *
 * Take the data from @buffer.
 *
 * Returns: the data of @buffer.
 */
gpointer
pinos_buffer_steal_data (PinosBuffer       *buffer,
                         gsize             *size)
{
  PinosStackBuffer *sb = PSB (buffer);
  gpointer data;

  g_return_val_if_fail (is_valid_buffer (buffer), 0);

  data = sb->data;
  if (size)
    *size = sb->size;

  if (sb->data != sb->free_data)
    g_free (sb->free_data);
  sb->data = NULL;
  sb->free_data = NULL;
  sb->size = 0;
  sb->max_size = 0;

  return data;
}

/**
 * pinos_buffer_steal_fds:
 * @buffer: a #PinosBuffer
 * @n_fds: number of fds
 *
 * Take the fds from @buffer.
 *
 * Returns: the fds of @buffer.
 */
gint *
pinos_buffer_steal_fds (PinosBuffer       *buffer,
                        gint              *n_fds)
{
  PinosStackBuffer *sb = PSB (buffer);
  gint *fds;

  g_return_val_if_fail (is_valid_buffer (buffer), 0);

  fds = sb->fds;
  if (n_fds)
   *n_fds = sb->n_fds;

  if (sb->fds != sb->free_fds)
    g_free (sb->free_fds);
  sb->fds = NULL;
  sb->free_fds = NULL;
  sb->n_fds = 0;
  sb->max_fds = 0;

  return fds;
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
 * @data: data to build into or %NULL to allocate
 * @max_data: allocated size of @data
 * @fds: memory for fds
 * @max_fds: maximum number of fds in @fds
 *
 * Initialize a stack allocated @builder and set the @version.
 */
void
pinos_buffer_builder_init_full (PinosBufferBuilder       *builder,
                                guint32                   version,
                                gpointer                  data,
                                gsize                     max_data,
                                gint                     *fds,
                                gint                      max_fds)
{
  struct stack_builder *sb = PPSB (builder);
  PinosStackHeader *sh;

  g_return_if_fail (builder != NULL);

  sb->magic = PPSB_MAGIC;

  if (max_data < sizeof (PinosStackHeader) || data == NULL) {
    sb->buf.max_size = sizeof (PinosStackHeader) + 128;
    sb->buf.data = g_malloc (sb->buf.max_size);
    sb->buf.free_data = sb->buf.data;
  } else {
    sb->buf.max_size = max_data;
    sb->buf.data = data;
    sb->buf.free_data = NULL;
  }
  sb->buf.size = sizeof (PinosStackHeader);

  sb->buf.fds = fds;
  sb->buf.max_fds = max_fds;
  sb->buf.n_fds = 0;
  sb->buf.free_fds = NULL;

  sh = sb->sh = sb->buf.data;
  sh->version = version;
  sh->flags = 0;
  sh->length = 0;

  sb->type = 0;
  sb->offset = 0;
}

/**
 * pinos_buffer_builder_set_flags:
 * @builder: a #PinosBufferBuilder
 * @flags: flags to set
 *
 * Set the flags on the buffer from @builder.
 */
void
pinos_buffer_builder_set_flags (PinosBufferBuilder *builder, PinosBufferFlags flags)
{
  struct stack_builder *sb = PPSB (builder);

  g_return_if_fail (is_valid_builder (builder));

  sb->sh->flags = flags;
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
  g_free (sb->buf.free_data);
  g_free (sb->buf.free_fds);
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

  sb->magic = 0;
  sb->sh->length = sb->buf.size - sizeof (PinosStackHeader);

  sbuf->magic = PSB_MAGIC;
  sbuf->data = sb->buf.data;
  sbuf->size = sb->buf.size;
  sbuf->max_size = sb->buf.max_size;
  sbuf->free_data = sb->buf.free_data;

  sbuf->fds = sb->buf.fds;
  sbuf->n_fds = sb->buf.n_fds;
  sbuf->max_fds = sb->buf.max_fds;
  sbuf->free_fds = sb->buf.free_fds;
}

/**
 * pinos_buffer_builder_add_fd:
 * @builder: a #PinosBufferBuilder
 * @fd: a valid fd
 *
 * Add the file descriptor @fd to @builder.
 *
 * Returns: the index of the file descriptor in @builder.
 */
gint
pinos_buffer_builder_add_fd (PinosBufferBuilder *builder,
                             int                 fd)
{
  struct stack_builder *sb = PPSB (builder);
  gint index;

  g_return_val_if_fail (is_valid_builder (builder), -1);
  g_return_val_if_fail (fd > 0, -1);

  if (sb->buf.n_fds >= sb->buf.max_fds) {
    sb->buf.max_fds += 8;
    sb->buf.free_fds = g_realloc (sb->buf.free_fds, sb->buf.max_fds * sizeof (int));
    sb->buf.fds = sb->buf.free_fds;
  }
  index = sb->buf.n_fds;
  sb->buf.fds[index] = fd;
  sb->buf.n_fds++;

  return index;
}

static gpointer
builder_ensure_size (struct stack_builder *sb, gsize size)
{
  if (sb->buf.size + size > sb->buf.max_size) {
    sb->buf.max_size = sb->buf.size + MAX (size, 1024);
    sb->buf.free_data = g_realloc (sb->buf.free_data, sb->buf.max_size);
    sb->sh = sb->buf.data = sb->buf.free_data;
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

/* header packets */
/**
 * pinos_buffer_iter_get_header:
 * @iter: a #PinosBufferIter
 * @header: a #PinosPacketHeader
 *
 * Get the #PinosPacketHeader. @iter must be positioned on a packet of
 * type #PINOS_PACKET_TYPE_HEADER
 *
 * Returns: %TRUE if @header contains valid data.
 */
gboolean
pinos_buffer_iter_parse_header (PinosBufferIter *iter,
                                PinosPacketHeader *header)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), FALSE);
  g_return_val_if_fail (si->type == PINOS_PACKET_TYPE_HEADER, FALSE);

  if (si->size < sizeof (PinosPacketHeader))
    return FALSE;

  memcpy (header, si->data, sizeof (*header));

  return TRUE;
}

/**
 * pinos_buffer_builder_add_header:
 * @builder: a #PinosBufferBuilder
 * @header: a #PinosPacketHeader
 *
 * Add a #PINOS_PACKET_TYPE_HEADER to @builder with data from @header.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_buffer_builder_add_header (PinosBufferBuilder *builder,
                                 PinosPacketHeader *header)
{
  struct stack_builder *sb = PPSB (builder);
  PinosPacketHeader *h;

  g_return_val_if_fail (is_valid_builder (builder), FALSE);

  h = builder_add_packet (sb, PINOS_PACKET_TYPE_HEADER, sizeof (PinosPacketHeader));
  memcpy (h, header, sizeof (*header));

  return TRUE;
}

/* fd-payload packets */
/**
 * pinos_buffer_iter_get_fd_payload:
 * @iter: a #PinosBufferIter
 * @payload: a #PinosPacketFDPayload
 *
 * Get the #PinosPacketFDPayload. @iter must be positioned on a packet of
 * type #PINOS_PACKET_TYPE_FD_PAYLOAD
 *
 * Returns: %TRUE if @payload contains valid data.
 */
gboolean
pinos_buffer_iter_parse_fd_payload (PinosBufferIter *iter,
                                    PinosPacketFDPayload *payload)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), FALSE);
  g_return_val_if_fail (si->type == PINOS_PACKET_TYPE_FD_PAYLOAD, FALSE);

  if (si->size < sizeof (PinosPacketFDPayload))
    return FALSE;

  memcpy (payload, si->data, sizeof (*payload));

  return TRUE;
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
  memcpy (p, payload, sizeof (*payload));

  return TRUE;
}

/**
 * pinos_buffer_iter_parse_release_fd_payload:
 * @iter: a #PinosBufferIter
 * @payload: a #PinosPacketReleaseFDPayload
 *
 * Parse a #PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD packet from @iter into @payload.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_buffer_iter_parse_release_fd_payload (PinosBufferIter      *iter,
                                            PinosPacketReleaseFDPayload *payload)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), FALSE);
  g_return_val_if_fail (si->type == PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD, FALSE);

  if (si->size < sizeof (PinosPacketReleaseFDPayload))
    return FALSE;

  memcpy (payload, si->data, sizeof (*payload));

  return TRUE;
}

/**
 * pinos_buffer_builder_add_release_fd_payload:
 * @builder: a #PinosBufferBuilder
 * @payload: a #PinosPacketReleaseFDPayload
 *
 * Add a #PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD payload in @payload to @builder.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_buffer_builder_add_release_fd_payload (PinosBufferBuilder   *builder,
                                             PinosPacketReleaseFDPayload *payload)
{
  struct stack_builder *sb = PPSB (builder);
  PinosPacketReleaseFDPayload *p;

  g_return_val_if_fail (is_valid_builder (builder), FALSE);

  p = builder_add_packet (sb,
                          PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD,
                          sizeof (PinosPacketReleaseFDPayload));
  memcpy (p, payload, sizeof (*payload));

  return TRUE;
}

/**
 * pinos_buffer_iter_parse_format_change:
 * @iter: a #PinosBufferIter
 * @payload: a #PinosPacketFormatChange
 *
 * Parse a #PINOS_PACKET_TYPE_FORMAT_CHANGE packet from @iter into @payload.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_buffer_iter_parse_format_change (PinosBufferIter         *iter,
                                       PinosPacketFormatChange *payload)
{
  struct stack_iter *si = PPSI (iter);
  char *p;

  g_return_val_if_fail (is_valid_iter (iter), FALSE);
  g_return_val_if_fail (si->type == PINOS_PACKET_TYPE_FORMAT_CHANGE, FALSE);

  if (si->size < 2)
    return FALSE;

  p = si->data;

  payload->id = *p++;
  payload->format = p;

  return TRUE;
}

/**
 * pinos_buffer_builder_add_format_change:
 * @builder: a #PinosBufferBuilder
 * @payload: a #PinosPacketFormatChange
 *
 * Add a #PINOS_PACKET_TYPE_FORMAT_CHANGE payload in @payload to @builder.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_buffer_builder_add_format_change (PinosBufferBuilder      *builder,
                                        PinosPacketFormatChange *payload)
{
  struct stack_builder *sb = PPSB (builder);
  gsize len;
  char *p;

  g_return_val_if_fail (is_valid_builder (builder), FALSE);

  /* id + format len + zero byte */
  len = 1 + strlen (payload->format) + 1;
  p = builder_add_packet (sb,
                          PINOS_PACKET_TYPE_FORMAT_CHANGE,
                          len);
  *p++ = payload->id;
  strcpy (p, payload->format);
  sb->sh->flags |= PINOS_BUFFER_FLAG_CONTROL;

  return TRUE;
}

/**
 * pinos_buffer_iter_parse_refresh_request:
 * @iter: a #PinosBufferIter
 * @payload: a #PinosPacketRefreshRequest
 *
 * Parse a #PINOS_PACKET_TYPE_REFRESH_REQUEST packet from @iter into @payload.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_buffer_iter_parse_refresh_request (PinosBufferIter           *iter,
                                         PinosPacketRefreshRequest *payload)
{
  struct stack_iter *si = PPSI (iter);

  g_return_val_if_fail (is_valid_iter (iter), FALSE);
  g_return_val_if_fail (si->type == PINOS_PACKET_TYPE_REFRESH_REQUEST, FALSE);

  if (si->size < sizeof (PinosPacketRefreshRequest))
    return FALSE;

  memcpy (payload, si->data, sizeof (*payload));

  return TRUE;
}

/**
 * pinos_buffer_builder_add_refresh_request:
 * @builder: a #PinosBufferBuilder
 * @payload: a #PinosPacketRefreshRequest
 *
 * Add a #PINOS_PACKET_TYPE_REFRESH_REQUEST payload in @payload to @builder.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_buffer_builder_add_refresh_request (PinosBufferBuilder        *builder,
                                          PinosPacketRefreshRequest *payload)
{
  struct stack_builder *sb = PPSB (builder);
  PinosPacketRefreshRequest *p;

  g_return_val_if_fail (is_valid_builder (builder), FALSE);

  p = builder_add_packet (sb,
                          PINOS_PACKET_TYPE_REFRESH_REQUEST,
                          sizeof (PinosPacketRefreshRequest));
  memcpy (p, payload, sizeof (*payload));

  return TRUE;
}
