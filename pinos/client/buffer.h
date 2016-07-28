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
#if 0
typedef struct _PinosBufferIter PinosBufferIter;
typedef struct _PinosBufferBuilder PinosBufferBuilder;

#define PINOS_BUFFER_VERSION 0

/**
 * PinosBufferFlags:
 * @PINOS_BUFFER_FLAG_NONE: no flags
 * @PINOS_BUFFER_FLAG_CONTROL: the buffer contains control info such
 *     as new format or properties.
 *
 * The possible buffer flags.
 */
typedef enum {
  PINOS_BUFFER_FLAG_NONE            = 0,
  PINOS_BUFFER_FLAG_CONTROL         = (1 << 0),
} PinosBufferFlags;

struct _PinosBuffer {
  PinosBuffer *next;
  /*< private >*/
  gsize x[16];
};

void               pinos_buffer_init_data        (PinosBuffer       *buffer,
                                                  gpointer           data,
                                                  gsize              size,
                                                  gint              *fds,
                                                  gint               n_fds);

PinosBuffer *      pinos_buffer_ref              (PinosBuffer       *buffer);
gboolean           pinos_buffer_unref            (PinosBuffer       *buffer);

guint32            pinos_buffer_get_version      (PinosBuffer       *buffer);
PinosBufferFlags   pinos_buffer_get_flags        (PinosBuffer       *buffer);
int                pinos_buffer_get_fd           (PinosBuffer       *buffer,
                                                  gint               index);

gpointer           pinos_buffer_steal_data       (PinosBuffer       *buffer,
                                                  gsize             *size);
gint *             pinos_buffer_steal_fds        (PinosBuffer       *buffer,
                                                  gint              *n_fds);


/**
 * PinosPacketType:
 * @PINOS_PACKET_TYPE_INVALID: invalid packet type, ignore
 * @PINOS_PACKET_TYPE_CONTINUATION: continuation packet, used internally to send
 *      commands using a shared memory region.
 * @PINOS_PACKET_TYPE_REGISTER_MEM: register memory region
 * @PINOS_PACKET_TYPE_RELEASE_MEM: release memory region
 * @PINOS_PACKET_TYPE_START: start transfer
 * @PINOS_PACKET_TYPE_STOP: stop transfer
 *
 * @PINOS_PACKET_TYPE_HEADER: common packet header
 * @PINOS_PACKET_TYPE_PROCESS_MEM: packet contains mem-payload. An mem-payload contains
 *      the media data as the index of a shared memory region
 * @PINOS_PACKET_TYPE_REUSE_MEM: when a memory region has been consumed and is ready to
 *      be reused.
 * @PINOS_PACKET_TYPE_FORMAT_CHANGE: a format change.
 * @PINOS_PACKET_TYPE_PROPERTY_CHANGE: one or more property changes.
 * @PINOS_PACKET_TYPE_REFRESH_REQUEST: ask for a new keyframe
 *
 * The possible packet types.
 */
typedef enum {
  PINOS_PACKET_TYPE_INVALID            = 0,

  PINOS_PACKET_TYPE_CONTINUATION,
  PINOS_PACKET_TYPE_ADD_MEM,
  PINOS_PACKET_TYPE_REMOVE_MEM,
  PINOS_PACKET_TYPE_START,
  PINOS_PACKET_TYPE_STREAMING,
  PINOS_PACKET_TYPE_STOP,
  PINOS_PACKET_TYPE_STOPPED,
  PINOS_PACKET_TYPE_DRAIN,
  PINOS_PACKET_TYPE_DRAINED,
  PINOS_PACKET_TYPE_HEADER,
  PINOS_PACKET_TYPE_PROCESS_MEM,
  PINOS_PACKET_TYPE_REUSE_MEM,
  PINOS_PACKET_TYPE_FORMAT_CHANGE,
  PINOS_PACKET_TYPE_PROPERTY_CHANGE,
  PINOS_PACKET_TYPE_REFRESH_REQUEST,
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
void               pinos_buffer_iter_end         (PinosBufferIter *iter);

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
                                                    guint32                  version,
                                                    gpointer                 data,
                                                    gsize                    max_data,
                                                    gint                    *fds,
                                                    gint                     max_fds);
#define pinos_buffer_builder_init_into(b,d,md,f,mf) pinos_buffer_builder_init_full(b, PINOS_BUFFER_VERSION,d,md,f,mf);
#define pinos_buffer_builder_init(b)                pinos_buffer_builder_init_into(b, NULL, 0, NULL, 0);

void               pinos_buffer_builder_set_flags  (PinosBufferBuilder *builder,
                                                    PinosBufferFlags    flags);
void               pinos_buffer_builder_clear      (PinosBufferBuilder *builder);
void               pinos_buffer_builder_end        (PinosBufferBuilder *builder,
                                                    PinosBuffer        *buffer);

gboolean           pinos_buffer_builder_add_empty  (PinosBufferBuilder   *builder,
                                                    PinosPacketType       type);

gint               pinos_buffer_builder_add_fd     (PinosBufferBuilder *builder,
                                                    int                 fd);
/* add-mem packets */
/**
 * PinosPacketAddMem:
 * @port: the port number
 * @id: the unique id of this memory block
 * @type: the memory block type
 * @fd_index: the index of the fd with the data
 * @offset: the offset of the data
 * @size: the size of the data
 *
 * A Packet that contains a memory block used for data transfer.
 */
typedef struct {
  guint32 port;
  guint32 id;
  guint32 type;
  gint32 fd_index;
  guint64 offset;
  guint64 size;
} PinosPacketAddMem;

gboolean           pinos_buffer_iter_parse_add_mem      (PinosBufferIter      *iter,
                                                         PinosPacketAddMem    *payload);
gboolean           pinos_buffer_builder_add_add_mem     (PinosBufferBuilder   *builder,
                                                         PinosPacketAddMem    *payload);

/* remove-mem packets */
/**
 * PinosPacketRemoveMem:
 * @port: the port number
 * @id: the unique id of the memory block
 *
 * Remove a memory block.
 */
typedef struct {
  guint32 port;
  guint32 id;
} PinosPacketRemoveMem;

gboolean           pinos_buffer_iter_parse_remove_mem   (PinosBufferIter      *iter,
                                                         PinosPacketRemoveMem *payload);
gboolean           pinos_buffer_builder_add_remove_mem  (PinosBufferBuilder   *builder,
                                                         PinosPacketRemoveMem *payload);

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
  guint32 port;
  guint32 flags;
  guint32 seq;
  gint64 pts;
  gint64 dts_offset;
} PinosPacketHeader;

gboolean           pinos_buffer_iter_parse_header      (PinosBufferIter    *iter,
                                                        PinosPacketHeader  *payload);
gboolean           pinos_buffer_builder_add_header     (PinosBufferBuilder *builder,
                                                        PinosPacketHeader  *payload);


/* process-mem packets */
/**
 * PinosPacketProcessMem:
 * @id: the mem index to process
 * @offset: the offset of the data
 * @size: the size of the data
 *
 * A Packet that contains data in an fd at @fd_index at @offset and with
 * @size.
 */
typedef struct {
  guint32 port;
  guint32 id;
  guint64 offset;
  guint64 size;
} PinosPacketProcessMem;

gboolean           pinos_buffer_iter_parse_process_mem  (PinosBufferIter       *iter,
                                                         PinosPacketProcessMem *payload);
gboolean           pinos_buffer_builder_add_process_mem (PinosBufferBuilder    *builder,
                                                         PinosPacketProcessMem *payload);

/* reuse-mem packets */
/**
 * PinosPacketReuseMem:
 * @id: the unique id of the memory block to reuse
 *
 * Release the payload with @id
 */
typedef struct {
  guint32 port;
  guint32 id;
  guint64 offset;
  guint64 size;
} PinosPacketReuseMem;

gboolean           pinos_buffer_iter_parse_reuse_mem  (PinosBufferIter      *iter,
                                                       PinosPacketReuseMem  *payload);
gboolean           pinos_buffer_builder_add_reuse_mem (PinosBufferBuilder   *builder,
                                                       PinosPacketReuseMem  *payload);


/* format change packets */
/**
 * PinosPacketFormatChange:
 * @id: the id of the new format
 * @format: the new format
 *
 * A new format.
 */
typedef struct {
  guint32 port;
  guint32 id;
  const gchar *format;
} PinosPacketFormatChange;

gboolean           pinos_buffer_iter_parse_format_change   (PinosBufferIter         *iter,
                                                            PinosPacketFormatChange *payload);
gboolean           pinos_buffer_builder_add_format_change  (PinosBufferBuilder      *builder,
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
  guint32 port;
  const gchar *key;
  const gchar *value;
} PinosPacketPropertyChange;

gboolean           pinos_buffer_iter_parse_property_change  (PinosBufferIter           *iter,
                                                             guint                      idx,
                                                             PinosPacketPropertyChange *payload);
gboolean           pinos_buffer_builder_add_property_change (PinosBufferBuilder        *builder,
                                                             PinosPacketPropertyChange *payload);

/* refresh request packets */
/**
 * PinosPacketRefreshRequest:
 * @last_id: last frame seen frame id
 * @request_type: the type of the request
 * @pts: the timestamp of the requested key frame, 0 = as soon as possible
 *
 * A refresh request packet. This packet is sent to trigger a new keyframe.
 */
typedef struct {
  guint32 port;
  guint32 last_id;
  guint32 request_type;
  gint64 pts;
} PinosPacketRefreshRequest;

gboolean           pinos_buffer_iter_parse_refresh_request  (PinosBufferIter      *iter,
                                                             PinosPacketRefreshRequest *payload);
gboolean           pinos_buffer_builder_add_refresh_request (PinosBufferBuilder   *builder,
                                                             PinosPacketRefreshRequest *payload);


#endif
#endif /* __PINOS_BUFFER_H__ */
