/* GStreamer
 * Copyright (C) <2012> Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __GST_BURST_CACHE_H__
#define __GST_BURST_CACHE_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_BURST_CACHE \
  (gst_burst_cache_get_type())
#define GST_BURST_CACHE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BURST_CACHE,GstBurstCache))
#define GST_BURST_CACHE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BURST_CACHE,GstBurstCacheClass))
#define GST_IS_BURST_CACHE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BURST_CACHE))
#define GST_IS_BURST_CACHE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BURST_CACHE))
#define GST_BURST_CACHE_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_BURST_CACHE, GstBurstCacheClass))

typedef struct _GstBurstCache GstBurstCache;
typedef struct _GstBurstCacheClass GstBurstCacheClass;
typedef struct _GstBurstCacheReader GstBurstCacheReader;

/**
 * GstBurstCacheRecover:
 * @GST_BURST_CACHE_RECOVER_NONE             : no recovering is done
 * @GST_BURST_CACHE_RECOVER_RESYNC_LATEST    : reader is moved to last buffer
 * @GST_BURST_CACHE_RECOVER_RESYNC_SOFT_LIMIT: reader is moved to the soft limit
 * @GST_BURST_CACHE_RECOVER_RESYNC_KEYFRAME  : reader is moved to latest keyframe
 *
 * Possible values for the recovery procedure to use when a reader consumes
 * data too slowly and has a backlog of more that soft-limit buffers.
 */
typedef enum
{
  GST_BURST_CACHE_RECOVER_NONE,
  GST_BURST_CACHE_RECOVER_RESYNC_LATEST,
  GST_BURST_CACHE_RECOVER_RESYNC_SOFT_LIMIT,
  GST_BURST_CACHE_RECOVER_RESYNC_KEYFRAME
} GstBurstCacheRecover;

/**
 * GstBurstCacheStart:
 * @GST_BURST_CACHE_START_LATEST              : reader receives most recent buffer
 * @GST_BURST_CACHE_START_NEXT_KEYFRAME       : reader receives next keyframe
 * @GST_BURST_CACHE_START_LATEST_KEYFRAME     : reader receives latest keyframe (burst)
 * @GST_BURST_CACHE_START_BURST               : reader receives specific amount of data
 * @GST_BURST_CACHE_START_BURST_KEYFRAME      : reader receives specific amount of data
 *                                              starting from latest keyframe
 * @GST_BURST_CACHE_START_BURST_WITH_KEYFRAME : reader receives specific amount of data from
 *                                              a keyframe, or if there is not enough data after
 *                                              the keyframe, starting before the keyframe
 *
 * This enum defines the selection of the first buffer that is sent
 * to a new reader.
 */
typedef enum
{
  GST_BURST_CACHE_START_LATEST,
  GST_BURST_CACHE_START_NEXT_KEYFRAME,
  GST_BURST_CACHE_START_LATEST_KEYFRAME,
  GST_BURST_CACHE_START_BURST,
  GST_BURST_CACHE_START_BURST_KEYFRAME,
  GST_BURST_CACHE_START_BURST_WITH_KEYFRAME
} GstBurstCacheStart;

/**
 * GstBurstCacheError:
 * @GST_BURST_CACHE_ERROR_NONE     : No error
 * @GST_BURST_CACHE_ERROR_SLOW     : reader is too slow
 * @GST_BURST_CACHE_ERROR_ERROR    : reader is in error
 * @GST_BURST_CACHE_ERROR_DUPLICATE: same reader added twice
 *
 * Error codes used in the reason GError.
 */
typedef enum
{
  GST_BURST_CACHE_ERROR_NONE        = 0,
  GST_BURST_CACHE_ERROR_SLOW        = 1,
  GST_BURST_CACHE_ERROR_ERROR       = 2,
  GST_BURST_CACHE_ERROR_DUPLICATE   = 3,
} GstBurstCacheError;

GQuark gst_burst_cache_error_quark (void);
#define GST_BURST_CACHE_ERROR       gst_burst_cache_error_quark()

/**
 * GstBurstCacheResult:
 * @GST_BURST_CACHE_RESULT_ERROR  : An error occured
 * @GST_BURST_CACHE_RESULT_OK     : No error
 * @GST_BURST_CACHE_RESULT_WAIT   : Wait for more buffers
 * @GST_BURST_CACHE_RESULT_EOS    : No more buffers
 *
 * Error codes used in the reason GError.
 */
typedef enum
{
  GST_BURST_CACHE_RESULT_ERROR       = 0,
  GST_BURST_CACHE_RESULT_OK          = 1,
  GST_BURST_CACHE_RESULT_WAIT        = 2,
  GST_BURST_CACHE_RESULT_EOS         = 3,
} GstBurstCacheResult;

/**
 * GstBurstCacheReaderCallback:
 * @cache: a #GstBurstCache
 * @reader: a #GstBurstCacheReader
 * @user_data: user data given when creating @reader
 *
 * Called when @reader in @cache has data. You can get the new data with
 * gst_burst_cache_get_buffer() from this callback or any other thread.
 */
typedef void (*GstBurstCacheReaderCallback)  (GstBurstCache *cache,
                                              GstBurstCacheReader *reader,
                                              gpointer user_data);

/**
 * GstBurstCacheReader:
 * @object: parent miniobject
 * @bufpos: position of this reader in the global queue
 * @draincount: the remaining number of buffers to drain or -1 if the
 *              reader is not draining.
 * @new_reader: this is a new reader
 * @discont: is the next buffer was discont
 * @reason: the reason why the reader is being removed
 *
 * structure for a reader
 */
struct _GstBurstCacheReader {
  GHook hook;

  gint bufpos;
  gint draincount;

  GstBurstCacheReaderCallback callback;
  gpointer user_data;
  GDestroyNotify notify;

  gboolean new_reader;
  gboolean discont;

  GError *reason;

  /* method to sync reader when connecting */
  GstBurstCacheStart start_method;
  GstFormat          min_format;
  guint64            min_value;
  GstFormat          max_format;
  guint64            max_value;

  /* stats */
  guint64 bytes_sent;
  guint64 dropped_buffers;
  guint64 avg_queue_size;
  guint64 first_buffer_ts;
  guint64 last_buffer_ts;

  guint64 add_time;
  guint64 remove_time;
  guint64 last_activity_time;
  guint64 timeout;

  gchar debug[30];              /* a debug string used in debug calls to
                                   identify the reader */
};

/**
 * GstBurstCache:
 * @parent: parent GObject
 * @lock: lock to protect @readers
 * @bufqueue: global queue of buffers
 * @readers: list of readers we are serving
 * @readers_cookie: Cookie to detect changes to @readers
 * @limit_format: the format of @limit_max and @@limit_soft_max
 * @limit_max: max units to queue for a reader
 * @limit_soft_max: max units a reader can lag before recovery starts
 * @recover: how to recover a lagging reader
 * @bytes_min: min number of bytes to queue
 * @time_min: min time to queue
 * @buffers_min: min number of buffers to queue
 * @bytes_to_serve: how much bytes we must serve
 * @bytes_served: how much bytes have we served
 * @bytes_queued: number of queued bytes
 * @time_queued: amount of queued time
 * @buffers_queued: number of queued buffers
 */
struct _GstBurstCache {
  GObject parent;

  /*< private >*/
  GRecMutex lock;
  GPtrArray *bufqueue;
  /* the readers */
  GHookList readers;
  guint readers_cookie;

  /* these values are used to check if a reader is reading fast
   * enough and to control recovery */
  GstFormat limit_format;
  gint64 limit_max;
  gint64 limit_soft_max;
  GstBurstCacheRecover recover;

  /* these values are used to control the amount of data
   * kept in the queues. It allows readers to perform a burst
   * on connect. */
  gint   bytes_min;
  gint64 time_min;
  gint   buffers_min;

  /* stats */
  gint bytes_queued;
  gint64 time_queued;
  gint buffers_queued;
};

/**
 * GstBurstCacheClass:
 * @parent_class: parent GObjectClass
 * @reader_ready: called when a reader has a new buffer available
 *
 * The GstBurstCache class structure.
 */
struct _GstBurstCacheClass {
  GObjectClass parent_class;
};

GType gst_burst_cache_get_type (void);
GType gst_burst_cache_reader_get_type (void);

GstBurstCache *         gst_burst_cache_new              (guint reader_size);

void                    gst_burst_cache_set_min_amount   (GstBurstCache *cache,
                                                          gint bytes_min,
                                                          gint64 time_min,
                                                          gint buffers_min);
void                    gst_burst_cache_get_min_amount   (GstBurstCache *cache,
                                                          gint *bytes_min,
                                                          gint64 *time_min,
                                                          gint *buffers_min);

void                    gst_burst_cache_set_limits       (GstBurstCache *cache,
                                                          GstFormat format,
                                                          gint64 max,
                                                          gint64 soft_max,
                                                          GstBurstCacheRecover recover);
void                    gst_burst_cache_get_limits       (GstBurstCache *cache,
                                                          GstFormat *format,
                                                          gint64 *max,
                                                          gint64 *soft_max,
                                                          GstBurstCacheRecover *recover);

void                    gst_burst_cache_queue_buffer     (GstBurstCache *cache,
                                                          GstBuffer *buffer);

GstBurstCacheReader *   gst_burst_cache_reader_new       (GstBurstCache *cache,
                                                          GstBurstCacheReaderCallback callback,
                                                          gpointer user_data,
                                                          GDestroyNotify notify);
gboolean                gst_burst_cache_reader_set_burst (GstBurstCacheReader *reader,
                                                          GstBurstCacheStart start_method,
                                                          GstFormat min_format, guint64 min_value,
                                                          GstFormat max_format, guint64 max_value);
void                    gst_burst_cache_reader_destroy   (GstBurstCacheReader *reader);

gboolean                gst_burst_cache_add_reader       (GstBurstCache *cache,
                                                          GstBurstCacheReader *reader);
gboolean                gst_burst_cache_remove_reader    (GstBurstCache *cache,
                                                          GstBurstCacheReader *reader,
                                                          gboolean drain);
gboolean                gst_burst_cache_error_reader     (GstBurstCache *cache,
                                                          GstBurstCacheReader *reader,
                                                          GError *error);

void                    gst_burst_cache_clear_readers    (GstBurstCache * cache);


GstBurstCacheResult     gst_burst_cache_get_buffer       (GstBurstCache *cache,
                                                          GstBurstCacheReader *reader,
                                                          GstBuffer **buffer);

G_END_DECLS

#endif /* __GST_BURST_CACHE_H__ */
