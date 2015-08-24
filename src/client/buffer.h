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

#ifndef __PINOS_BUFFER_H__
#define __PINOS_BUFFER_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PinosBuffer PinosBuffer;
typedef struct _PinosBufferInfo PinosBufferInfo;
typedef struct _PinosPacketIter PinosPacketIter;
typedef struct _PinosPacketBuilder PinosPacketBuilder;

#define PINOS_BUFFER_VERSION 0

typedef struct {
  guint32 flags;
  guint32 seq;
  gint64 pts;
  gint64 dts_offset;
} PinosBufferHeader;

struct _PinosBuffer {
  /*< private >*/
  gsize x[16];
};

void               pinos_buffer_init_take_data   (PinosBuffer       *buffer,
                                                  gpointer           data,
                                                  gsize              size,
                                                  GSocketControlMessage *message);

void               pinos_buffer_clear            (PinosBuffer       *buffer);

const PinosBufferHeader *
                   pinos_buffer_get_header       (PinosBuffer       *buffer,
                                                  guint32           *version);
int                pinos_buffer_get_fd           (PinosBuffer       *buffer,
                                                  gint               index,
                                                  GError           **error);
GSocketControlMessage *
                   pinos_buffer_get_socket_control_message  (PinosBuffer *buffer);

gsize              pinos_buffer_get_size         (PinosBuffer       *buffer);
void               pinos_buffer_store            (PinosBuffer       *buffer,
                                                  gpointer           data);


typedef enum {
  PINOS_PACKET_TYPE_INVALID           = 0,

  PINOS_PACKET_TYPE_FD_PAYLOAD        = 1,
  PINOS_PACKET_TYPE_FORMAT_CHANGE     = 2,
  PINOS_PACKET_TYPE_PROPERTY_CHANGE   = 3,
} PinosPacketType;


/* iterating packets */
struct _PinosPacketIter {
  /*< private >*/
  gsize x[16];
};

void               pinos_packet_iter_init_full   (PinosPacketIter *iter,
                                                  PinosBuffer     *buffer,
                                                  guint32          version);
#define pinos_packet_iter_init(i,b)   pinos_packet_iter_init_full(i,b, PINOS_BUFFER_VERSION);

gboolean           pinos_packet_iter_next        (PinosPacketIter *iter);

PinosPacketType    pinos_packet_iter_get_type    (PinosPacketIter *iter);
gpointer           pinos_packet_iter_get_data    (PinosPacketIter *iter, gsize *size);

/**
 * PinosPacketBuilder:
 */
struct _PinosPacketBuilder {
  /*< private >*/
  gsize x[16];
};

void               pinos_packet_builder_init_full (PinosPacketBuilder      *builder,
                                                   guint32                  version,
                                                   const PinosBufferHeader *header);
#define pinos_packet_builder_init(b,h)   pinos_packet_builder_init_full(b, PINOS_BUFFER_VERSION,h);

void               pinos_packet_builder_end       (PinosPacketBuilder *builder,
                                                   PinosBuffer        *buffer);

/* fd-payload packets */
/**
 * PinosPacketFDPayload:
 * @fd_index: the index of the fd with the data
 * @offset: the offset of the data
 * @size: the size of the data
 *
 * A Packet that contains data in an fd at @fd_index at @offset and with
 * @size.
 */
typedef struct {
  gint32 fd_index;
  gint64 offset;
  gint64 size;
} PinosPacketFDPayload;

void               pinos_packet_iter_parse_fd_payload   (PinosPacketIter      *iter,
                                                         PinosPacketFDPayload *payload);
gboolean           pinos_packet_builder_add_fd_payload  (PinosPacketBuilder   *builder,
                                                         gint64 offset, gint64 size, int fd,
                                                         GError              **error);

#endif /* __PINOS_BUFFER_H__ */

