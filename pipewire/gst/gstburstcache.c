/* GStreamer
 * Copyright (C) <2004> Thomas Vander Stichele <thomas at apestaart dot org>
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

/**
 * SECTION:gstburstcache
 * @short_description: caches and implements burst-on-connect of buffers
 *
 * GstBurstCache keeps a cache of buffers up to a configurable limit and replays
 * this cache for newly added readers. This can be used to implement
 * burst-on-connect for various network scenarios such as TCP or UDP.
 *
 * A new cache is created with gst_burst_cache_new(), which requires a size of
 * the structure to hold the reader information.
 *
 * New buffers are put in the cache with gst_burst_cache_queue_buffer(). Old
 * buffers will be removed from the cache. With gst_burst_cache_set_min_amount()
 * one can control the amount of data in time/bytes/buffers to keep in the
 * cache.
 *
 * New readers can be constructed with gst_burst_cache_reader_new(). This will
 * allocate a new reader structure with the configured size when the cache was
 * constructed. A callback needs to be provided that will be called when new
 * data is available for the reader.
 *
 * The caller can configure the reader with gst_burst_cache_reader_set_burst().
 * The start_method property will define which buffer in the cahced buffers will
 * be sent first to the reader. Readers can be sent the most recent buffer
 * (which might not be decodable by the reader if it is not a keyframe), the
 * next keyframe received in the cache (which can take some time depending on
 * the keyframe rate), or the last received keyframe (which will cause a simple
 * burst-on-connect). GstBurstCache will always keep at least one keyframe in
 * its internal cache.
 *
 * There are additional values for the start_method to allow finer control over
 * burst-on-connect behaviour. By selecting the 'burst' method a minimum burst
 * size can be chosen, 'burst-keyframe' additionally requires that the burst
 * begin with a keyframe, and 'burst-with-keyframe' attempts to burst beginning
 * with a keyframe, but will prefer a minimum burst size even if it requires
 * not starting with a keyframe.
 *
 * When a reader is created and configured, it can be added to the cache with
 * gst_burst_cache_add_reader(). This will trigger the callback when new data is
 * available for the reader. The reader should call gst_burst_cache_get_buffer()
 * to retrieve the available buffer until the function returns
 * GST_BURST_CACHE_RESULT_WAIT, in which case it should wait for the callback again.
 * This makes it possible to implement a push or pull model for retrieving data
 * from the cache.
 *
 * If the reader does not call gst_burst_cache_get_buffer() fast enough, it will
 * start to lag. With gst_burst_cache_set_limits() you can configure how much a
 * reader is allowed to lag. You can configure a soft limit and a hard limit in
 * and format.
 *
 * A reader with a lag of at least soft-max enters the recovery procedure which
 * is controlled with the recover property. A recover policy of NONE will do
 * nothing, RESYNC_LATEST will send the most recently received buffer as the
 * next buffer for the reader, RESYNC_SOFT_LIMIT positions the reader to the
 * soft limit in the buffer queue and RESYNC_KEYFRAME positions the reader at
 * the most recent keyframe in the buffer queue.
 *
 * When the reader is lagging more that the soft-limit, its recovery
 * procedure will be started, which usually will make it drop buffers to catch
 * up. When the hard limit is reached, the reader is removed from the cache.
 *
 * A reader can be removed from the cache with gst_burst_cache_remove_reader().
 *
 * When a reader is in error, gst_burst_cache_error_reader() must be called,
 * which will cause the reader to be removed from the cache.
 *
 * When a reader is freed, its GHook destroy function will be called with the
 * GHook data. You can install a custom function and data to be notified.
 *
 * Last reviewed on 2012-11-09 (1.0.3)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstburstcache.h"

#define DEFAULT_LIMIT_FORMAT            GST_FORMAT_BUFFERS
#define DEFAULT_LIMIT_MAX               -1
#define DEFAULT_LIMIT_SOFT_MAX          -1
#define DEFAULT_TIME_MIN                -1
#define DEFAULT_BYTES_MIN               -1
#define DEFAULT_BUFFERS_MIN             -1
#define DEFAULT_RECOVER                 GST_BURST_CACHE_RECOVER_NONE

enum
{
  PROP_0,
  PROP_LAST
};

GQuark gst_burst_cache_error_quark (void)
{
  static GQuark quark;
  if (!quark)
    quark = g_quark_from_static_string ("gst-burst-cache-error-quark");
  return quark;
}

GST_DEBUG_CATEGORY_STATIC (burstcache_debug);
#define GST_CAT_DEFAULT (burstcache_debug)

#define gst_burst_cache_parent_class parent_class
G_DEFINE_TYPE (GstBurstCache, gst_burst_cache, G_TYPE_OBJECT);

#define CACHE_LOCK_INIT(cache)       (g_rec_mutex_init(&(cache)->lock))
#define CACHE_LOCK_CLEAR(cache)      (g_rec_mutex_clear(&(cache)->lock))
#define CACHE_LOCK(cache)            (g_rec_mutex_lock(&(cache)->lock))
#define CACHE_UNLOCK(cache)          (g_rec_mutex_unlock(&(cache)->lock))

#define VALUE_INVALID ((guint64)-1)

static void gst_burst_cache_finalize (GObject * object);

G_DEFINE_POINTER_TYPE (GstBurstCacheReader, gst_burst_cache_reader);

static gint find_keyframe (GstBurstCache * cache, gint idx, gint direction);
#define find_next_keyframe(s,i)	find_keyframe(s,i,1)
#define find_prev_keyframe(s,i)	find_keyframe(s,i,-1)
static gboolean is_keyframe (GstBurstCache * cache, GstBuffer * buffer);

static gint get_buffers_max (GstBurstCache * cache, GstFormat format,
    gint64 max);
static gint gst_burst_cache_recover_reader (GstBurstCache * cache,
    GstBurstCacheReader * reader);
static gboolean find_limits (GstBurstCache * cache, gint * min_idx,
    gint bytes_min, gint buffers_min, gint64 time_min, gint * max_idx,
    gint bytes_max, gint buffers_max, gint64 time_max);

static void
gst_burst_cache_class_init (GstBurstCacheClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = gst_burst_cache_finalize;

  GST_DEBUG_CATEGORY_INIT (burstcache_debug, "burstcache", 0, "GstBurstCache");
}

static void
gst_burst_cache_init (GstBurstCache * this)
{
  CACHE_LOCK_INIT (this);
  this->bufqueue = g_ptr_array_new ();
  this->limit_format = DEFAULT_LIMIT_FORMAT;
  this->limit_max = DEFAULT_LIMIT_MAX;
  this->limit_soft_max = DEFAULT_LIMIT_SOFT_MAX;
  this->time_min = DEFAULT_TIME_MIN;
  this->bytes_min = DEFAULT_BYTES_MIN;
  this->buffers_min = DEFAULT_BUFFERS_MIN;
  this->recover = DEFAULT_RECOVER;
}


static void
gst_burst_cache_finalize (GObject * object)
{
  GstBurstCache *this;

  this = GST_BURST_CACHE (object);

  g_hook_list_clear (&this->readers);

  g_ptr_array_foreach (this->bufqueue, (GFunc) gst_buffer_unref, NULL);
  g_ptr_array_free (this->bufqueue, TRUE);
  CACHE_LOCK_CLEAR (this);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/**
 * gst_burst_cache_new:
 * @reader_size: the size of the hook
 *
 * Make a new burst cache. @hook_size specifies the size of the data structure
 * that is kep for each client and must be at least
 * sizeof(GstBurstCacheReader).
 *
 * Returns: a new #GstBurstCache
 */
GstBurstCache *
gst_burst_cache_new (guint reader_size)
{
  GstBurstCache *cache;

  g_return_val_if_fail (reader_size >= sizeof (GstBurstCacheReader), NULL);

  cache = g_object_new (GST_TYPE_BURST_CACHE, NULL);
  g_hook_list_init (&cache->readers, reader_size);

  return cache;
}

/**
 * gst_burst_cache_set_min_amount:
 * @cache: a #GstBurstCache
 * @bytes_min: minimum amount to cache in bytes
 * @time_min: minimum amount to cache in time
 * @buffers_min: minimum amount to cache in buffers
 *
 * Set the minimum amount of data that @cache should keep around. Values can be
 * specified in bytes, time and buffers. A value of -1 sets unlimited caching.
 */
void
gst_burst_cache_set_min_amount (GstBurstCache * cache, gint bytes_min,
    gint64 time_min, gint buffers_min)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  cache->bytes_min = bytes_min;
  cache->time_min = time_min;
  cache->buffers_min = buffers_min;
}

/**
 * gst_burst_cache_get_min_amount:
 * @cache: a #GstBurstCache
 * @bytes_min: (out) (allow-none): result in bytes
 * @time_min: (out) (allow-none): result in time
 * @buffers_min: (out) (allow-none): result in buffers
 *
 * Get the minimum amount of data that @cache keeps around.
 */
void
gst_burst_cache_get_min_amount (GstBurstCache * cache, gint * bytes_min,
    gint64 * time_min, gint * buffers_min)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  if (bytes_min)
    *bytes_min = cache->bytes_min;
  if (time_min)
    *time_min = cache->time_min;
  if (buffers_min)
    *buffers_min = cache->buffers_min;
}

/**
 * gst_burst_cache_set_limits:
 * @cache: a #GstBurstCache
 * @format: format of @max and @soft_max
 * @max: maximum lag for a reader
 * @soft_max: maximum lag for a reader before recovery is performed
 * @recover: a #GstBurstCacheRecover
 *
 * Set the limits for readers. When a reader lags more than @soft_max behind the
 * most recent buffer, the receovery procedure @recovery is started for the
 * client. If the client lags up to @max, it will be removed from @cache.
 */
void
gst_burst_cache_set_limits (GstBurstCache * cache, GstFormat format,
    gint64 max, gint64 soft_max, GstBurstCacheRecover recover)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  cache->limit_format = format;
  cache->limit_max = max;
  cache->limit_soft_max = soft_max;
  cache->recover = recover;
}

/**
 * gst_burst_cache_get_limits:
 * @cache: a #GstBurstCache
 * @format: (out) (allow-none): result format of @max and @soft_max
 * @max: (out) (allow-none): result maximum lag for a reader
 * @soft_max: (out) (allow-none): result maximum lag for a reader before
 *        recovery is performed
 * @recover: (out) (allow-none): result #GstBurstCacheRecover
 *
 * Get the reader limits. See gst_burst_cache_set_limits().
 */
void
gst_burst_cache_get_limits (GstBurstCache * cache, GstFormat * format,
    gint64 * max, gint64 * soft_max, GstBurstCacheRecover * recover)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  if (format)
    *format = cache->limit_format;
  if (max)
    *max = cache->limit_max;
  if (soft_max)
    *soft_max = cache->limit_soft_max;
  if (recover)
    *recover = cache->recover;
}

/**
 * gst_burst_cache_reader_destroy:
 * @reader: a #GstBurstCacheReader
 *
 * Cleanup the memory of @reader.
 */
void
gst_burst_cache_reader_destroy (GstBurstCacheReader * reader)
{
  if (reader->reason)
    g_error_free (reader->reason);
  if (reader->notify)
    reader->notify (reader->user_data);
}

/**
 * gst_burst_cache_reader_new:
 * @cache: a #GstBurstCache
 * @callback: a #GstBurstCacheReaderCallback
 * @user_data: user data
 * @notify: a #GDestroyNotify for @user_data
 *
 * Make a new #GstBurstCacheReader. When data becomes available for the reader,
 * @callback will be called with @user_data.
 *
 * Returns: a new #GstBurstCacheReader.
 */
GstBurstCacheReader *
gst_burst_cache_reader_new (GstBurstCache * cache,
    GstBurstCacheReaderCallback callback, gpointer user_data,
    GDestroyNotify notify)
{
  GstBurstCacheReader *reader;

  reader = (GstBurstCacheReader *) g_hook_alloc (&cache->readers);

  reader->hook.data = reader;
  reader->hook.destroy = (GDestroyNotify) gst_burst_cache_reader_destroy;

  reader->bufpos = -1;
  reader->draincount = -1;
  reader->new_reader = TRUE;
  reader->discont = FALSE;

  reader->callback = callback;
  reader->user_data = user_data;
  reader->notify = notify;

  reader->reason = NULL;

  reader->start_method = GST_BURST_CACHE_START_LATEST;

  reader->bytes_sent = 0;
  reader->dropped_buffers = 0;
  reader->avg_queue_size = 0;
  reader->first_buffer_ts = GST_CLOCK_TIME_NONE;
  reader->last_buffer_ts = GST_CLOCK_TIME_NONE;

  /* update start time */
  reader->add_time = g_get_real_time ();
  reader->remove_time = 0;
  /* set last activity time to add time */
  reader->last_activity_time = reader->add_time;

  return reader;
}

/**
 * gst_burst_cache_reader_set_burst:
 * @reader: a #GstBurstCacheReader
 * @start_method: a #GstBurstCacheStart
 * @min_format: format of @min_value
 * @min_value: minimum burst amount
 * @max_format: format of @max_value
 * @max_value: maximum burst amount
 *
 * Set the burst parameters for @reader. @start_method defines where to position
 * the reader in the cache. At least @min_value of data and at most @max_value
 * of data will be sent to the new client.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_burst_cache_reader_set_burst (GstBurstCacheReader * reader,
    GstBurstCacheStart start_method, GstFormat min_format, guint64 min_value,
    GstFormat max_format, guint64 max_value)
{
  /* do limits check if we can */
  if (min_format == max_format) {
    if (max_value != VALUE_INVALID && min_value != VALUE_INVALID && max_value < min_value)
      return FALSE;
  }

  reader->start_method = start_method;
  reader->min_format = min_format;
  reader->min_value = min_value;
  reader->max_format = max_format;
  reader->max_value = max_value;

  return TRUE;
}

static gboolean
is_keyframe (GstBurstCache * cache, GstBuffer * buffer)
{
  if (GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
    return FALSE;
  } else if (!GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_HEADER)) {
    return TRUE;
  }
  return FALSE;
}

/* find the keyframe in the list of buffers starting the
 * search from @idx. @direction as -1 will search backwards,
 * 1 will search forwards.
 * Returns: the index or -1 if there is no keyframe after idx.
 */
static gint
find_keyframe (GstBurstCache * cache, gint idx, gint direction)
{
  gint i, len, result;

  /* take length of queued buffers */
  len = cache->bufqueue->len;

  /* assume we don't find a keyframe */
  result = -1;

  /* then loop over all buffers to find the first keyframe */
  for (i = idx; i >= 0 && i < len; i += direction) {
    GstBuffer *buf;

    buf = g_ptr_array_index (cache->bufqueue, i);
    if (is_keyframe (cache, buf)) {
      GST_LOG_OBJECT (cache, "found keyframe at %d from %d, direction %d",
          i, idx, direction);
      result = i;
      break;
    }
  }
  return result;
}

/* Get the number of buffers from the buffer queue needed to satisfy
 * the maximum max in the configured units.
 * If units are not BUFFERS, and there are insufficient buffers in the
 * queue to satify the limit, return len(queue) + 1 */
static gint
get_buffers_max (GstBurstCache * cache, GstFormat format, gint64 max)
{
  switch (format) {
    case GST_FORMAT_BUFFERS:
      return max;
    case GST_FORMAT_TIME:
    {
      GstBuffer *buf;
      int i;
      int len;
      gint64 diff;
      GstClockTime first = GST_CLOCK_TIME_NONE;

      len = cache->bufqueue->len;

      for (i = 0; i < len; i++) {
        buf = g_ptr_array_index (cache->bufqueue, i);
        if (GST_BUFFER_TIMESTAMP_IS_VALID (buf)) {
          if (first == GST_CLOCK_TIME_NONE)
            first = GST_BUFFER_TIMESTAMP (buf);

          diff = first - GST_BUFFER_TIMESTAMP (buf);

          if (diff > max)
            return i + 1;
        }
      }
      return len + 1;
    }
    case GST_FORMAT_BYTES:
    {
      GstBuffer *buf;
      int i;
      int len;
      gint acc = 0;

      len = cache->bufqueue->len;

      for (i = 0; i < len; i++) {
        buf = g_ptr_array_index (cache->bufqueue, i);
        acc += gst_buffer_get_size (buf);

        if (acc > max)
          return i + 1;
      }
      return len + 1;
    }
    default:
      return max;
  }
}

/* find the positions in the buffer queue where *_min and *_max
 * is satisfied
 */
/* count the amount of data in the buffers and return the index
 * that satifies the given limits.
 *
 * Returns: index @idx in the buffer queue so that the given limits are
 * satisfied. TRUE if all the limits could be satisfied, FALSE if not
 * enough data was in the queue.
 *
 * FIXME, this code might not work if any of the units is in buffers...
 */
static gboolean
find_limits (GstBurstCache * cache,
    gint * min_idx, gint bytes_min, gint buffers_min, gint64 time_min,
    gint * max_idx, gint bytes_max, gint buffers_max, gint64 time_max)
{
  GstClockTime first, time;
  gint i, len, bytes;
  gboolean result, max_hit;

  /* take length of queue */
  len = cache->bufqueue->len;

  /* this must hold */
  g_assert (len > 0);

  GST_LOG_OBJECT (cache,
      "bytes_min %d, buffers_min %d, time_min %" GST_TIME_FORMAT
      ", bytes_max %d, buffers_max %d, time_max %" GST_TIME_FORMAT, bytes_min,
      buffers_min, GST_TIME_ARGS (time_min), bytes_max, buffers_max,
      GST_TIME_ARGS (time_max));

  /* do the trivial buffer limit test */
  if (buffers_min != -1 && len < buffers_min) {
    *min_idx = len - 1;
    *max_idx = len - 1;
    return FALSE;
  }

  result = FALSE;
  /* else count bytes and time */
  first = -1;
  bytes = 0;
  /* unset limits */
  *min_idx = -1;
  *max_idx = -1;
  max_hit = FALSE;

  i = 0;
  /* loop through the buffers, when a limit is ok, mark it
   * as -1, we have at least one buffer in the queue. */
  do {
    GstBuffer *buf;

    /* if we checked all min limits, update result */
    if (bytes_min == -1 && time_min == -1 && *min_idx == -1) {
      /* don't go below 0 */
      *min_idx = MAX (i - 1, 0);
    }
    /* if we reached one max limit break out */
    if (max_hit) {
      /* i > 0 when we get here, we subtract one to get the position
       * of the previous buffer. */
      *max_idx = i - 1;
      /* we have valid complete result if we found a min_idx too */
      result = *min_idx != -1;
      break;
    }
    buf = g_ptr_array_index (cache->bufqueue, i);

    bytes += gst_buffer_get_size (buf);

    /* take timestamp and save for the base first timestamp */
    if ((time = GST_BUFFER_TIMESTAMP (buf)) != GST_CLOCK_TIME_NONE) {
      GST_LOG_OBJECT (cache, "Ts %" GST_TIME_FORMAT " on buffer",
          GST_TIME_ARGS (time));
      if (first == GST_CLOCK_TIME_NONE)
        first = time;

      /* increase max usage if we did not fill enough. Note that
       * buffers are sorted from new to old, so the first timestamp is
       * bigger than the next one. */
      if (time_min != -1 && first - time >= (guint64) time_min)
        time_min = -1;
      if (time_max != -1 && first - time >= (guint64) time_max)
        max_hit = TRUE;
    } else {
      GST_LOG_OBJECT (cache, "No timestamp on buffer");
    }
    /* time is OK or unknown, check and increase if not enough bytes */
    if (bytes_min != -1) {
      if (bytes >= bytes_min)
        bytes_min = -1;
    }
    if (bytes_max != -1) {
      if (bytes >= bytes_max) {
        max_hit = TRUE;
      }
    }
    i++;
  }
  while (i < len);

  /* if we did not hit the max or min limit, set to buffer size */
  if (*max_idx == -1)
    *max_idx = len - 1;
  /* make sure min does not exceed max */
  if (*min_idx == -1)
    *min_idx = *max_idx;

  return result;
}

/* parse the unit/value pair and assign it to the result value of the
 * right type, leave the other values untouched
 *
 * Returns: FALSE if the unit is unknown or undefined. TRUE otherwise.
 */
static gboolean
assign_value (GstFormat format, guint64 value, gint * bytes, gint * buffers,
    GstClockTime * time)
{
  gboolean res = TRUE;

  /* set only the limit of the given format to the given value */
  switch (format) {
    case GST_FORMAT_BUFFERS:
      *buffers = (gint) value;
      break;
    case GST_FORMAT_TIME:
      *time = value;
      break;
    case GST_FORMAT_BYTES:
      *bytes = (gint) value;
      break;
    case GST_FORMAT_UNDEFINED:
    default:
      res = FALSE;
      break;
  }
  return res;
}

/* count the index in the buffer queue to satisfy the given unit
 * and value pair starting from buffer at index 0.
 *
 * Returns: TRUE if there was enough data in the queue to satisfy the
 * burst values. @idx contains the index in the buffer that contains enough
 * data to satisfy the limits or the last buffer in the queue when the
 * function returns FALSE.
 */
static gboolean
count_burst_unit (GstBurstCache * cache, gint * min_idx,
    GstFormat min_format, guint64 min_value, gint * max_idx,
    GstFormat max_format, guint64 max_value)
{
  gint bytes_min = -1, buffers_min = -1;
  gint bytes_max = -1, buffers_max = -1;
  GstClockTime time_min = GST_CLOCK_TIME_NONE, time_max = GST_CLOCK_TIME_NONE;

  assign_value (min_format, min_value, &bytes_min, &buffers_min, &time_min);
  assign_value (max_format, max_value, &bytes_max, &buffers_max, &time_max);

  return find_limits (cache, min_idx, bytes_min, buffers_min, time_min,
      max_idx, bytes_max, buffers_max, time_max);
}

/* decide where in the current buffer queue this new reader should start
 * receiving buffers from.
 * This function is called whenever a reader is added and has not yet
 * received a buffer.
 */
static void
handle_new_reader (GstBurstCache * cache, GstBurstCacheReader * reader)
{
  gint position;

  GST_DEBUG_OBJECT (cache,
      "%s new reader, deciding where to start in queue", reader->debug);
  GST_DEBUG_OBJECT (cache, "queue is currently %d buffers long",
      cache->bufqueue->len);

  switch (reader->start_method) {
    case GST_BURST_CACHE_START_LATEST:
      /* no syncing, we are happy with whatever the reader is going to get */
      position = reader->bufpos;
      GST_DEBUG_OBJECT (cache,
          "%s BURST_CACHE_START_LATEST, position %d", reader->debug, position);
      break;
    case GST_BURST_CACHE_START_NEXT_KEYFRAME:
    {
      /* if one of the new buffers (between reader->bufpos and 0) in the queue
       * is a key frame, we can proceed, otherwise we need to keep waiting */
      GST_LOG_OBJECT (cache,
          "%s new reader, bufpos %d, waiting for keyframe",
          reader->debug, reader->bufpos);

      position = find_prev_keyframe (cache, reader->bufpos);
      if (position != -1) {
        GST_DEBUG_OBJECT (cache,
            "%s BURST_CACHE_START_NEXT_KEYFRAME: position %d", reader->debug,
            position);
        break;
      }

      /* reader is not on a keyframe, need to skip these buffers and
       * wait some more */
      GST_LOG_OBJECT (cache,
          "%s new reader, skipping buffer(s), no keyframe found",
          reader->debug);
      reader->bufpos = -1;
      break;
    }
    case GST_BURST_CACHE_START_LATEST_KEYFRAME:
    {
      GST_DEBUG_OBJECT (cache, "%s BURST_CACHE_START_LATEST_KEYFRAME",
          reader->debug);

      /* for new readers we initially scan the complete buffer queue for
       * a keyframe when a buffer is added. If we don't find a keyframe,
       * we need to wait for the next keyframe and so we change the reader's
       * start method to GST_BURST_CACHE_START_NEXT_KEYFRAME.
       */
      position = find_next_keyframe (cache, 0);
      if (position != -1) {
        GST_DEBUG_OBJECT (cache,
            "%s BURST_CACHE_START_LATEST_KEYFRAME: position %d", reader->debug,
            position);
        break;
      }

      GST_DEBUG_OBJECT (cache,
          "%s BURST_CACHE_START_LATEST_KEYFRAME: no keyframe found, "
          "switching to BURST_CACHE_START_NEXT_KEYFRAME", reader->debug);
      /* throw reader to the waiting state */
      reader->bufpos = -1;
      /* and make reader sync to next keyframe */
      reader->start_method = GST_BURST_CACHE_START_NEXT_KEYFRAME;
      break;
    }
    case GST_BURST_CACHE_START_BURST:
    {
      gboolean ok;
      gint max;

      /* move to the position where we satisfy the reader's burst
       * parameters. If we could not satisfy the parameters because there
       * is not enough data, we just send what we have (which is in position).
       * We use the max value to limit the search
       */
      ok = count_burst_unit (cache, &position, reader->min_format,
          reader->min_value, &max, reader->max_format, reader->max_value);
      GST_DEBUG_OBJECT (cache,
          "%s BURST_CACHE_START_BURST: burst_unit returned %d, position %d",
          reader->debug, ok, position);

      GST_LOG_OBJECT (cache, "min %d, max %d", position, max);

      /* we hit the max and it is below the min, use that then */
      if (max != -1 && max <= position) {
        position = MAX (max - 1, 0);
        GST_DEBUG_OBJECT (cache,
            "%s BURST_CACHE_START_BURST: position above max, taken down to %d",
            reader->debug, position);
      }
      break;
    }
    case GST_BURST_CACHE_START_BURST_KEYFRAME:
    {
      gint min_idx, max_idx;
      gint next_keyframe, prev_keyframe;

      /* BURST_KEYFRAME:
       *
       * _always_ start sending a keyframe to the reader. We first search
       * a keyframe between min/max limits. If there is none, we send it the
       * last keyframe before min. If there is none, the behaviour is like
       * NEXT_KEYFRAME.
       */
      /* gather burst limits */
      count_burst_unit (cache, &min_idx, reader->min_format,
          reader->min_value, &max_idx, reader->max_format, reader->max_value);

      GST_LOG_OBJECT (cache, "min %d, max %d", min_idx, max_idx);

      /* first find a keyframe after min_idx */
      next_keyframe = find_next_keyframe (cache, min_idx);
      if (next_keyframe != -1 && next_keyframe < max_idx) {
        /* we have a valid keyframe and it's below the max */
        GST_LOG_OBJECT (cache, "found keyframe in min/max limits");
        position = next_keyframe;
        break;
      }

      /* no valid keyframe, try to find one below min */
      prev_keyframe = find_prev_keyframe (cache, min_idx);
      if (prev_keyframe != -1) {
        GST_WARNING_OBJECT (cache,
            "using keyframe below min in BURST_KEYFRAME start mode");
        position = prev_keyframe;
        break;
      }

      /* no prev keyframe or not enough data  */
      GST_WARNING_OBJECT (cache,
          "no prev keyframe found in BURST_KEYFRAME start mode, waiting for next");

      /* throw reader to the waiting state */
      reader->bufpos = -1;
      /* and make reader sync to next keyframe */
      reader->start_method = GST_BURST_CACHE_START_NEXT_KEYFRAME;
      position = -1;
      break;
    }
    case GST_BURST_CACHE_START_BURST_WITH_KEYFRAME:
    {
      gint min_idx, max_idx;
      gint next_keyframe;

      /* BURST_WITH_KEYFRAME:
       *
       * try to start sending a keyframe to the reader. We first search
       * a keyframe between min/max limits. If there is none, we send it the
       * amount of data up 'till min.
       */
      /* gather enough data to burst */
      count_burst_unit (cache, &min_idx, reader->min_format,
          reader->min_value, &max_idx, reader->max_format, reader->max_value);

      GST_LOG_OBJECT (cache, "min %d, max %d", min_idx, max_idx);

      /* first find a keyframe after min_idx */
      next_keyframe = find_next_keyframe (cache, min_idx);
      if (next_keyframe != -1 && next_keyframe < max_idx) {
        /* we have a valid keyframe and it's below the max */
        GST_LOG_OBJECT (cache, "found keyframe in min/max limits");
        position = next_keyframe;
        break;
      }

      /* no keyframe, send data from min_idx */
      GST_WARNING_OBJECT (cache, "using min in BURST_WITH_KEYFRAME start mode");

      /* make sure we don't go over the max limit */
      if (max_idx != -1 && max_idx <= min_idx) {
        position = MAX (max_idx - 1, 0);
      } else {
        position = min_idx;
      }

      break;
    }
    default:
      g_warning ("unknown start method %d", reader->start_method);
      position = reader->bufpos;
      break;
  }

  if (position >= 0) {
    /* we got a valid spot in the queue */
    reader->new_reader = FALSE;
    reader->bufpos = position;
    /* signal that the reader is ready */
    reader->callback (cache, reader, reader->user_data);
  }
}

/**
 * gst_burst_cache_add_reader:
 * @cache: a #GstBurstCache
 * @reader: a #GstBurstCacheReader
 *
 * Add @reader to @cache.
 *
 * Returns: %TRUE when @reader could be added
 */
gboolean
gst_burst_cache_add_reader (GstBurstCache * cache, GstBurstCacheReader * reader)
{
  g_return_val_if_fail (GST_IS_BURST_CACHE (cache), FALSE);
  g_return_val_if_fail (reader != NULL, FALSE);
  g_return_val_if_fail (reader->new_reader, FALSE);

  /* do limits check if we can */
  if (reader->min_format == reader->max_format) {
    if (reader->max_value != VALUE_INVALID && reader->min_value != VALUE_INVALID &&
        reader->max_value < reader->min_value)
      goto wrong_limits;
  }

  CACHE_LOCK (cache);
  handle_new_reader (cache, reader);
  /* we can add the handle now */
  g_hook_prepend (&cache->readers, (GHook *) reader);
  cache->readers_cookie++;
  CACHE_UNLOCK (cache);

  return TRUE;

  /* errors */
wrong_limits:
  {
    GST_WARNING_OBJECT (cache,
        "%s wrong values min =%" G_GUINT64_FORMAT ", max=%"
        G_GUINT64_FORMAT ", unit %d specified when adding reader",
        reader->debug, reader->min_value, reader->max_value,
        reader->min_format);
    return FALSE;
  }
}

/* should be called with the readerslock held.  */
static void
gst_burst_cache_remove_reader_link (GstBurstCache * cache,
    GstBurstCacheReader * reader, gboolean remove, GError * reason)
{
  if (!G_HOOK_IS_VALID (reader))
    goto was_removing;

  GST_DEBUG_OBJECT (cache, "%s removing reader %p: (%s)",
      reader->debug, reader, reason ? reason->message : "Unknown reason");

  /* set reader to invalid position while being removed */
  reader->bufpos = -1;
  reader->reason = reason;
  reader->remove_time = g_get_real_time ();

  cache->readers_cookie++;
  if (remove)
    g_hook_destroy_link (&cache->readers, (GHook *) reader);

  return;

  /* ERRORS */
was_removing:
  {
    GST_WARNING_OBJECT (cache, "%s reader is already being removed",
        reader->debug);
    if (reason)
      g_error_free (reason);
    return;
  }
}

/**
 * gst_burst_cache_remove_reader:
 * @cache: a #GstBurstCache
 * @reader: a #GstBurstCacheReader
 * @drain: drain flag
 *
 * Remove @reader from @cache. When @drain is %TRUE all remaining data
 * in the cache will be sent to the reader before it is removed.
 *
 * Returns: %TRUE on success
 */
gboolean
gst_burst_cache_remove_reader (GstBurstCache * cache,
    GstBurstCacheReader * reader, gboolean drain)
{
  g_return_val_if_fail (GST_IS_BURST_CACHE (cache), FALSE);
  g_return_val_if_fail (reader != NULL, FALSE);

  GST_DEBUG_OBJECT (cache, "%s removing reader", reader->debug);

  CACHE_LOCK (cache);
  if (!G_HOOK_IS_VALID (reader))
    goto not_valid;

  if (drain) {
    if (reader->draincount == -1) {
      /* take the position of the reader as the number of buffers left to drain.
       * If the reader was at position -1, we drain 0 buffers, 0 == drain 1
       * buffer, etc... This will mark reader as draining. We can not remove the
       * reader right away because it might have some buffers to drain in its
       * queue. */
      reader->draincount = reader->bufpos + 1;
    } else {
      GST_INFO_OBJECT (cache, "%s Reader already draining", reader->debug);
    }
  } else {
    gst_burst_cache_remove_reader_link (cache, reader, TRUE,
        g_error_new (GST_BURST_CACHE_ERROR, GST_BURST_CACHE_ERROR_NONE, "User requested remove"));
  }
  CACHE_UNLOCK (cache);

  return TRUE;

  /* ERRORS */
not_valid:
  {
    GST_WARNING_OBJECT (cache, "reader %s not found!", reader->debug);
    CACHE_UNLOCK (cache);
    return FALSE;
  }
}

/**
 * gst_burst_cache_error_reader:
 * @cache: a #GstBurstCache
 * @reader: a #GstBurstCacheReader
 * @error: (transfer full): a #GError
 *
 * Remove @reader from @cache and set the reason to @error. Ownership is taken
 * of @error.
 *
 * Returns: %TRUE on success
 */
gboolean
gst_burst_cache_error_reader (GstBurstCache * cache,
    GstBurstCacheReader * reader, GError * error)
{
  g_return_val_if_fail (GST_IS_BURST_CACHE (cache), FALSE);
  g_return_val_if_fail (reader != NULL, FALSE);

  GST_DEBUG_OBJECT (cache, "%s error reader", reader->debug);

  CACHE_LOCK (cache);
  if (!G_HOOK_IS_VALID (reader))
    goto not_valid;

  if (error == NULL)
    error = g_error_new (GST_BURST_CACHE_ERROR, GST_BURST_CACHE_ERROR_ERROR, "Unknown error");

  GST_WARNING_OBJECT (cache, "%s reader %p error, removing: %s",
      reader->debug, reader, error->message);

  gst_burst_cache_remove_reader_link (cache, reader, TRUE, error);
  CACHE_UNLOCK (cache);

  return TRUE;

  /* ERRORS */
not_valid:
  {
    GST_WARNING_OBJECT (cache, "reader %s not found!", reader->debug);
    CACHE_UNLOCK (cache);
    return FALSE;
  }
}

static gboolean
remove_hook (GstBurstCacheReader * reader, GstBurstCache * cache)
{
  gst_burst_cache_remove_reader_link (cache, reader, FALSE,
      g_error_new (GST_BURST_CACHE_ERROR, GST_BURST_CACHE_ERROR_NONE, "User requested clear"));

  /* FALSE to remove */
  return FALSE;
}

/**
 * gst_burst_cache_remove_readers:
 * @cache: a #GstBurstCache
 *
 * Remove all readers from @cache.
 */
void
gst_burst_cache_remove_readers (GstBurstCache * cache)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  GST_DEBUG_OBJECT (cache, "removing all readers");

  CACHE_LOCK (cache);
  g_hook_list_marshal_check (&cache->readers, TRUE, (GHookCheckMarshaller)
      remove_hook, cache);
  CACHE_UNLOCK (cache);
}

/* calculate the new position for a reader after recovery. This function
 * does not update the reader position but merely returns the required
 * position.
 */
static gint
gst_burst_cache_recover_reader (GstBurstCache * cache,
    GstBurstCacheReader * reader)
{
  gint newbufpos;

  GST_WARNING_OBJECT (cache,
      "%s reader %p is lagging at %d, recover using policy %d",
      reader->debug, reader, reader->bufpos, cache->recover);

  switch (cache->recover) {
    case GST_BURST_CACHE_RECOVER_NONE:
      /* do nothing, reader will catch up or get kicked out when it reaches
       * the hard max */
      newbufpos = reader->bufpos;
      break;
    case GST_BURST_CACHE_RECOVER_RESYNC_LATEST:
      /* move to beginning of queue */
      newbufpos = -1;
      break;
    case GST_BURST_CACHE_RECOVER_RESYNC_SOFT_LIMIT:
      /* move to beginning of soft max */
      newbufpos =
          get_buffers_max (cache, cache->limit_format, cache->limit_soft_max);
      break;
    case GST_BURST_CACHE_RECOVER_RESYNC_KEYFRAME:
      /* find keyframe in buffers, we search backwards to find the
       * closest keyframe relative to what this reader already received. */
      newbufpos = MIN ((gint) (cache->bufqueue->len - 1),
          get_buffers_max (cache, cache->limit_format,
              cache->limit_soft_max) - 1);

      while (newbufpos >= 0) {
        GstBuffer *buf;

        buf = g_ptr_array_index (cache->bufqueue, newbufpos);
        if (is_keyframe (cache, buf)) {
          /* found a buffer that is not a delta unit */
          break;
        }
        newbufpos--;
      }
      break;
    default:
      /* unknown recovery procedure */
      newbufpos =
          get_buffers_max (cache, cache->limit_format, cache->limit_soft_max);
      break;
  }
  return newbufpos;
}

typedef struct
{
  GstBurstCache *cache;
  GstBurstCacheClass *klass;
  GstClockTime now;
  gint max_buffer_usage;
  gint max_buffers;
  gint soft_max_buffers;
} QueueHookData;

static gboolean
queue_hook (GstBurstCacheReader * reader, QueueHookData * data)
{
  GstBurstCache *cache = data->cache;

  /* move reader forwards */
  reader->bufpos++;

  GST_LOG_OBJECT (cache, "%s reader %p at position %d",
      reader->debug, reader, reader->bufpos);

  /* check soft max if needed, recover reader */
  if (data->soft_max_buffers > 0 && reader->bufpos >= data->soft_max_buffers) {
    gint newpos;

    newpos = gst_burst_cache_recover_reader (cache, reader);
    if (newpos != reader->bufpos) {
      reader->dropped_buffers += reader->bufpos - newpos;
      reader->bufpos = newpos;
      reader->discont = TRUE;
      GST_INFO_OBJECT (cache, "%s reader %p position reset to %d",
          reader->debug, reader, reader->bufpos);
    } else {
      GST_INFO_OBJECT (cache,
          "%s reader %p not recovering position", reader->debug, reader);
    }
  }

  /* check hard max */
  if (data->max_buffers > 0 && reader->bufpos >= data->max_buffers)
    goto hit_limit;

  /* check timeout */
  if (reader->timeout > 0 && data->now - reader->last_activity_time >
      reader->timeout)
    goto timeout;

  if (reader->new_reader) {
    handle_new_reader (cache, reader);
  } else if (reader->bufpos == 0) {
    /* reader changed from -1 to 0, we can send data to this reader now. */
    reader->callback (cache, reader, reader->user_data);
  }

  /* keep track of maximum buffer usage */
  if (reader->bufpos > data->max_buffer_usage) {
    data->max_buffer_usage = reader->bufpos;
  }

  return TRUE;

  /* ERRORS */
hit_limit:
  {
    GST_WARNING_OBJECT (cache, "%s reader %p is too slow, removing",
        reader->debug, reader);
    gst_burst_cache_remove_reader_link (cache, reader, FALSE,
        g_error_new (GST_BURST_CACHE_ERROR, GST_BURST_CACHE_ERROR_SLOW, "Reader is too slow"));
    /* remove reader */
    return FALSE;
  }
timeout:
  {
    GST_WARNING_OBJECT (cache, "%s reader %p timeout, removing",
        reader->debug, reader);
    gst_burst_cache_remove_reader_link (cache, reader, FALSE,
        g_error_new (GST_BURST_CACHE_ERROR, GST_BURST_CACHE_ERROR_SLOW, "Reader timed out"));
    /* remove reader */
    return FALSE;
  }
}

/**
 * gst_burst_cache_queue_buffer:
 * @cache: a #GstBurstCache
 * @buffer: a #GstBuffer
 *
 * Queue @buffer in @cache. Older and unused buffers will be removed from
 * @cache.
 */
void
gst_burst_cache_queue_buffer (GstBurstCache * cache, GstBuffer * buffer)
{
  gint queuelen;
  gint i;
  QueueHookData data;

  g_return_if_fail (GST_IS_BURST_CACHE (cache));
  g_return_if_fail (buffer != NULL);

  data.now = g_get_real_time ();

  data.klass = GST_BURST_CACHE_GET_CLASS (cache);

  CACHE_LOCK (cache);
  /* add buffer to queue */
  g_ptr_array_insert (cache->bufqueue, 0, buffer);
  queuelen = cache->bufqueue->len;

  data.cache = cache;

  if (cache->limit_max > 0)
    data.max_buffers =
        get_buffers_max (cache, cache->limit_format, cache->limit_max);
  else
    data.max_buffers = -1;

  if (cache->limit_soft_max > 0)
    data.soft_max_buffers =
        get_buffers_max (cache, cache->limit_format, cache->limit_soft_max);
  else
    data.soft_max_buffers = -1;

  GST_LOG_OBJECT (cache, "Using max %d, softmax %d", data.max_buffers,
      data.soft_max_buffers);

  /* After adding the buffer, we update all reader positions in the queue. If
   * a reader moves over the soft max, we start the recovery procedure for this
   * slow reader. If it goes over the hard max, it is put into the slow list
   * and removed.  */
  data.max_buffer_usage = 0;

  g_hook_list_marshal_check (&cache->readers, TRUE, (GHookCheckMarshaller)
      queue_hook, &data);

  /* make sure we respect bytes-min, buffers-min and time-min when they are set */
  {
    gint usage, max;

    GST_LOG_OBJECT (cache,
        "extending queue %d to respect time_min %" GST_TIME_FORMAT
        ", bytes_min %d, buffers_min %d", data.max_buffer_usage,
        GST_TIME_ARGS (cache->time_min), cache->bytes_min, cache->buffers_min);

    /* get index where the limits are ok, we don't really care if all limits
     * are ok, we just queue as much as we need. We also don't compare against
     * the max limits. */
    find_limits (cache, &usage, cache->bytes_min, cache->buffers_min,
        cache->time_min, &max, -1, -1, -1);

    data.max_buffer_usage = MAX (data.max_buffer_usage, usage + 1);
    GST_LOG_OBJECT (cache, "extended queue to %d", data.max_buffer_usage);
  }

  /* now look for start points and make sure there is at least one
   * keyframe point in the queue. */
  {
    /* no point in searching beyond the queue length */
    gint limit = queuelen;

    /* no point in searching beyond the soft-max if any. */
    if (data.soft_max_buffers > 0) {
      limit = MIN (limit, data.soft_max_buffers);
    }
    GST_LOG_OBJECT (cache,
        "extending queue to include start point, now at %d, limit is %d",
        data.max_buffer_usage, limit);

    for (i = 0; i < limit; i++) {
      GstBuffer *buf;

      buf = g_ptr_array_index (cache->bufqueue, i);
      if (is_keyframe (cache, buf)) {
        /* found a sync frame, now extend the buffer usage to
         * include at least this frame. */
        data.max_buffer_usage = MAX (data.max_buffer_usage, i);
        break;
      }
    }
    GST_LOG_OBJECT (cache, "max buffer usage is now %d", data.max_buffer_usage);
  }

  GST_LOG_OBJECT (cache, "len %d, usage %d", queuelen, data.max_buffer_usage);

  /* nobody is referencing units after max_buffer_usage so we can
   * remove them from the queue. We remove them in reverse order as
   * this is the most optimal for GArray. */
  for (i = queuelen - 1; i > data.max_buffer_usage; i--) {
    GstBuffer *old;

    /* queue exceeded max size */
    queuelen--;
    old = g_ptr_array_remove_index (cache->bufqueue, i);

    /* unref tail buffer */
    gst_buffer_unref (old);
  }
  /* save for stats */
  cache->buffers_queued = data.max_buffer_usage;
  CACHE_UNLOCK (cache);
}

/**
 * gst_burst_cache_remove_buffers:
 * @cache: a #GstBurstCache
 *
 * Remove all buffers from @cache.
 */
void
gst_burst_cache_remove_buffers (GstBurstCache * cache)
{
  g_return_if_fail (GST_IS_BURST_CACHE (cache));

  CACHE_LOCK (cache);
  g_ptr_array_foreach (cache->bufqueue, (GFunc) gst_buffer_unref, NULL);
  g_ptr_array_set_size (cache->bufqueue, 0);
  CACHE_UNLOCK (cache);
}

/**
 * gst_burst_cache_get_buffer:
 * @cache: a #GstBurstCache
 * @reader: a #GstBurstCacheReader
 * @buffer: a #GstBuffer
 *
 * Get the next buffer for @reader in @cache.
 *
 * Returns: #GST_BURST_CACHE_RESULT_OK when a buffer is available.
 * #GST_BURST_CACHE_RESULT_WAIT is returned when no buffers are available, the
 * caller should wait for the callback signal before attempting to get a
 * buffer again. #GST_BURST_CACHE_RESULT_EOS is returned when the client has
 * received all buffers and is ready to be removed.
 */
GstBurstCacheResult
gst_burst_cache_get_buffer (GstBurstCache * cache, GstBurstCacheReader * reader,
    GstBuffer ** buffer)
{
  GstBuffer *buf;
  GstClockTime timestamp;

  g_return_val_if_fail (GST_IS_BURST_CACHE (cache),
      GST_BURST_CACHE_RESULT_ERROR);
  g_return_val_if_fail (reader != NULL, GST_BURST_CACHE_RESULT_ERROR);
  g_return_val_if_fail (buffer != NULL, GST_BURST_CACHE_RESULT_ERROR);

  CACHE_LOCK (cache);
  if (reader->bufpos == -1)
    goto no_data_yet;

  /* we drained all remaining buffers, no need to get a new one */
  if (reader->draincount == 0)
    goto drained;

  /* grab buffer */
  buf = g_ptr_array_index (cache->bufqueue, reader->bufpos);
  reader->bufpos--;

  /* update stats */
  timestamp = GST_BUFFER_TIMESTAMP (buf);
  if (reader->first_buffer_ts == GST_CLOCK_TIME_NONE)
    reader->first_buffer_ts = timestamp;
  if (timestamp != GST_CLOCK_TIME_NONE)
    reader->last_buffer_ts = timestamp;

  /* decrease draincount */
  if (reader->draincount != -1)
    reader->draincount--;

  GST_LOG_OBJECT (cache, "%s reader %p at position %d",
      reader->debug, reader, reader->bufpos);

  *buffer = gst_buffer_ref (buf);
  CACHE_UNLOCK (cache);

  return GST_BURST_CACHE_RESULT_OK;

  /* ERRORS */
no_data_yet:
  {
    GST_DEBUG_OBJECT (cache, "%s no data available", reader->debug);
    CACHE_UNLOCK (cache);
    return GST_BURST_CACHE_RESULT_WAIT;
  }
drained:
  {
    GST_DEBUG_OBJECT (cache, "%s drained", reader->debug);
    CACHE_UNLOCK (cache);
    return GST_BURST_CACHE_RESULT_EOS;
  }
}
