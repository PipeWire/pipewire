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
typedef struct _PinosBufferIter PinosBufferIter;
typedef struct _PinosBufferBuilder PinosBufferBuilder;

#define PINOS_BUFFER_VERSION 0

struct _PinosBuffer {
  /*< private >*/
  gsize x[16];
};

void               pinos_buffer_init_data        (PinosBuffer       *buffer,
                                                  gpointer           data,
                                                  gsize              size,
                                                  GSocketControlMessage *message);

void               pinos_buffer_clear            (PinosBuffer       *buffer);

guint32            pinos_buffer_get_version      (PinosBuffer       *buffer);
int                pinos_buffer_get_fd           (PinosBuffer       *buffer,
                                                  gint               index,
                                                  GError           **error);

gpointer           pinos_buffer_steal            (PinosBuffer       *buffer,
                                                  gsize             *size,
                                                  GSocketControlMessage **message);


/**
 * PinosPacketType:
 * @PINOS_PACKET_TYPE_INVALID: invalid packet type, ignore
 * @PINOS_PACKET_TYPE_CONTINUATION: continuation packet, used internally to send
 *      commands using a shared memory region.
 * @PINOS_PACKET_TYPE_HEADER: common packet header
 * @PINOS_PACKET_TYPE_FD_PAYLOAD: packet contains fd-payload. An fd-payload contains
 *      the media data as a file descriptor
 * @PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD: packet contains release fd-payload. Notifies
 *      that a previously received fd-payload is no longer in use.
 * @PINOS_PACKET_TYPE_FORMAT_CHANGE: a format change.
 * @PINOS_PACKET_TYPE_PROPERTY_CHANGE: one or more property changes.
 *
 * The possible packet types.
 */
typedef enum {
  PINOS_PACKET_TYPE_INVALID            = 0,

  PINOS_PACKET_TYPE_CONTINUATION       = 1,
  PINOS_PACKET_TYPE_HEADER             = 2,
  PINOS_PACKET_TYPE_FD_PAYLOAD         = 3,
  PINOS_PACKET_TYPE_RELEASE_FD_PAYLOAD = 4,
  PINOS_PACKET_TYPE_FORMAT_CHANGE      = 5,
  PINOS_PACKET_TYPE_PROPERTY_CHANGE    = 6,
} PinosPacketType;


/* iterating packets */
struct _PinosBufferIter {
  /*< private >*/
  gsize x[16];
};

void               pinos_buffer_iter_init_full   (PinosBufferIter *iter,
                                                  PinosBuffer     *buffer,
                                                  guint32          version);
#define pinos_buffer_iter_init(i,b)   pinos_buffer_iter_init_full(i,b, PINOS_BUFFER_VERSION);

gboolean           pinos_buffer_iter_next        (PinosBufferIter *iter);

PinosPacketType    pinos_buffer_iter_get_type    (PinosBufferIter *iter);
gpointer           pinos_buffer_iter_get_data    (PinosBufferIter *iter, gsize *size);

/**
 * PinosBufferBuilder:
 */
struct _PinosBufferBuilder {
  /*< private >*/
  gsize x[16];
};

void               pinos_buffer_builder_init_full  (PinosBufferBuilder      *builder,
                                                    guint32                  version);
#define pinos_buffer_builder_init(b)   pinos_buffer_builder_init_full(b, PINOS_BUFFER_VERSION);

void               pinos_buffer_builder_clear      (PinosBufferBuilder *builder);
void               pinos_buffer_builder_end        (PinosBufferBuilder *builder,
                                                    PinosBuffer        *buffer);

gint               pinos_buffer_builder_add_fd     (PinosBufferBuilder *builder,
                                                    int                 fd,
                                                    GError            **error);
/* header packets */
/**
 * PinosPacketHeader
 * @flags: header flags
 * @seq: sequence number
 * @pts: presentation timestamp in nanoseconds
 * @dts_offset: offset to presentation timestamp in nanoseconds to get decode timestamp
 *
 * A Packet that contains the header.
 */
typedef struct {
  guint32 flags;
  guint32 seq;
  gint64 pts;
  gint64 dts_offset;
} PinosPacketHeader;

gboolean           pinos_buffer_iter_parse_header      (PinosBufferIter    *iter,
                                                        PinosPacketHeader  *header);
gboolean           pinos_buffer_builder_add_header     (PinosBufferBuilder *builder,
                                                        PinosPacketHeader  *header);

/* fd-payload packets */
/**
 * PinosPacketFDPayload:
 * @id: the unique id of this payload
 * @fd_index: the index of the fd with the data
 * @offset: the offset of the data
 * @size: the size of the data
 *
 * A Packet that contains data in an fd at @fd_index at @offset and with
 * @size.
 */
typedef struct {
  guint32 id;
  gint32 fd_index;
  guint64 offset;
  guint64 size;
} PinosPacketFDPayload;

gboolean           pinos_buffer_iter_parse_fd_payload   (PinosBufferIter      *iter,
                                                         PinosPacketFDPayload *payload);
gboolean           pinos_buffer_builder_add_fd_payload  (PinosBufferBuilder   *builder,
                                                         PinosPacketFDPayload *payload);

/* release fd-payload packets */
/**
 * PinosPacketReleaseFDPayload:
 * @id: the unique id of the fd-payload to release
 *
 * Release the payload with @id
 */
typedef struct {
  guint32 id;
} PinosPacketReleaseFDPayload;

gboolean           pinos_buffer_iter_parse_release_fd_payload   (PinosBufferIter      *iter,
                                                                 PinosPacketReleaseFDPayload *payload);
gboolean           pinos_buffer_builder_add_release_fd_payload  (PinosBufferBuilder   *builder,
                                                                 PinosPacketReleaseFDPayload *payload);


/* format change packets */
/**
 * PinosPacketFormatChange:
 * @id: the id of the new format
 * @format: the new format
 *
 * A new format.
 */
typedef struct {
  guint8 id;
  gchar *format;
} PinosPacketFormatChange;

gboolean           pinos_buffer_iter_parse_format_change   (PinosBufferIter      *iter,
                                                            PinosPacketFormatChange *payload);
gboolean           pinos_buffer_builder_add_format_change  (PinosBufferBuilder   *builder,
                                                            PinosPacketFormatChange *payload);


/* property change packets */
/**
 * PinosPacketPropertyChange:
 * @key: the key of the property
 * @value: the new value
 *
 * A new property change.
 */
typedef struct {
  gchar *key;
  gchar *value;
} PinosPacketPropertyChange;

gboolean           pinos_buffer_iter_parse_property_change  (PinosBufferIter      *iter,
                                                             guint                 idx,
                                                             PinosPacketPropertyChange *payload);
gboolean           pinos_buffer_builder_add_property_change (PinosBufferBuilder   *builder,
                                                             PinosPacketPropertyChange *payload);


#endif /* __PINOS_BUFFER_H__ */
