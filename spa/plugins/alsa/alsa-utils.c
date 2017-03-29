#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>
#include <limits.h>
#include <sys/timerfd.h>

#include <lib/debug.h>
#include "alsa-utils.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error (state->log, msg ": %s", snd_strerror(err)); return err; }

static void alsa_on_fd_events (SpaSource *source);

static int
spa_alsa_open (SpaALSAState *state)
{
  int err;
  SpaALSAProps *props = &state->props;

  if (state->opened)
    return 0;

  CHECK (snd_output_stdio_attach (&state->output, stderr, 0), "attach failed");

  spa_log_info (state->log, "ALSA device open '%s'", props->device);
  CHECK (snd_pcm_open (&state->hndl,
                       props->device,
                       state->stream,
                       SND_PCM_NONBLOCK |
                       SND_PCM_NO_AUTO_RESAMPLE |
                       SND_PCM_NO_AUTO_CHANNELS |
                       SND_PCM_NO_AUTO_FORMAT), "open failed");


  state->timerfd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  state->opened = true;

  return 0;
}

int
spa_alsa_close (SpaALSAState *state)
{
  int err = 0;

  if (!state->opened)
    return 0;

  spa_log_info (state->log, "Device closing");
  CHECK (snd_pcm_close (state->hndl), "close failed");

  close (state->timerfd);
  state->opened = false;

  return err;
}

typedef struct {
  off_t format_offset;
  snd_pcm_format_t format;
} FormatInfo;

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(fmt)  offsetof(Type, audio_format. fmt ## _OE)
#define _FORMAT_BE(fmt)  offsetof(Type, audio_format. fmt)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(fmt)  offsetof(Type, audio_format. fmt)
#define _FORMAT_BE(fmt)  offsetof(Type, audio_format. fmt ## _OE)
#endif

static const FormatInfo format_info[] =
{
  { offsetof(Type, audio_format.UNKNOWN),       SND_PCM_FORMAT_UNKNOWN },
  { offsetof(Type, audio_format.S8),            SND_PCM_FORMAT_S8 },
  { offsetof(Type, audio_format.U8),            SND_PCM_FORMAT_U8 },
  { _FORMAT_LE (S16),                           SND_PCM_FORMAT_S16_LE },
  { _FORMAT_BE (S16),                           SND_PCM_FORMAT_S16_BE },
  { _FORMAT_LE (U16),                           SND_PCM_FORMAT_U16_LE },
  { _FORMAT_BE (U16),                           SND_PCM_FORMAT_U16_BE },
  { _FORMAT_LE (S24_32),                        SND_PCM_FORMAT_S24_LE },
  { _FORMAT_BE (S24_32),                        SND_PCM_FORMAT_S24_BE },
  { _FORMAT_LE (U24_32),                        SND_PCM_FORMAT_U24_LE },
  { _FORMAT_BE (U24_32),                        SND_PCM_FORMAT_U24_BE },
  { _FORMAT_LE (S24),                           SND_PCM_FORMAT_S24_3LE },
  { _FORMAT_BE (S24),                           SND_PCM_FORMAT_S24_3BE },
  { _FORMAT_LE (U24),                           SND_PCM_FORMAT_U24_3LE },
  { _FORMAT_BE (U24),                           SND_PCM_FORMAT_U24_3BE },
  { _FORMAT_LE (S32),                           SND_PCM_FORMAT_S32_LE },
  { _FORMAT_BE (S32),                           SND_PCM_FORMAT_S32_BE },
  { _FORMAT_LE (U32),                           SND_PCM_FORMAT_U32_LE },
  { _FORMAT_BE (U32),                           SND_PCM_FORMAT_U32_BE },
};

static snd_pcm_format_t
spa_alsa_format_to_alsa (Type *map, uint32_t format)
{
  int i;

  for (i = 0; i < SPA_N_ELEMENTS (format_info); i++) {
    uint32_t f = *SPA_MEMBER (map, format_info[i].format_offset, uint32_t);
    if (f == format)
      return format_info[i].format;
  }
  return SND_PCM_FORMAT_UNKNOWN;
}

int
spa_alsa_set_format (SpaALSAState *state, SpaAudioInfo *fmt, SpaPortFormatFlags flags)
{
  unsigned int rrate, rchannels;
  snd_pcm_uframes_t period_size;
  int err, dir;
  snd_pcm_hw_params_t *params;
  snd_pcm_format_t format;
  SpaAudioInfoRaw *info = &fmt->info.raw;
  snd_pcm_t *hndl;
  unsigned int periods;
  SpaALSAProps *props = &state->props;

  if ((err = spa_alsa_open (state)) < 0)
    return err;

  hndl = state->hndl;

  snd_pcm_hw_params_alloca (&params);
  /* choose all parameters */
  CHECK (snd_pcm_hw_params_any (hndl, params), "Broken configuration for playback: no configurations available");
  /* set hardware resampling */
  CHECK (snd_pcm_hw_params_set_rate_resample (hndl, params, 0), "set_rate_resample");
  /* set the interleaved read/write format */
  CHECK (snd_pcm_hw_params_set_access(hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

  /* disable ALSA wakeups, we use a timer */
  if (snd_pcm_hw_params_can_disable_period_wakeup (params))
    CHECK (snd_pcm_hw_params_set_period_wakeup (hndl, params, 0), "set_period_wakeup");

  /* set the sample format */
  format = spa_alsa_format_to_alsa (&state->type, info->format);
  spa_log_info (state->log, "Stream parameters are %iHz, %s, %i channels", info->rate, snd_pcm_format_name(format), info->channels);
  CHECK (snd_pcm_hw_params_set_format (hndl, params, format), "set_format");

  /* set the count of channels */
  rchannels = info->channels;
  CHECK (snd_pcm_hw_params_set_channels_near (hndl, params, &rchannels), "set_channels");
  if (rchannels != info->channels) {
    spa_log_info (state->log, "Channels doesn't match (requested %u, get %u", info->channels, rchannels);
    if (flags & SPA_PORT_FORMAT_FLAG_NEAREST)
      info->channels = rchannels;
    else
      return -EINVAL;
  }

  /* set the stream rate */
  rrate = info->rate;
  CHECK (snd_pcm_hw_params_set_rate_near (hndl, params, &rrate, 0), "set_rate_near");
  if (rrate != info->rate) {
    spa_log_info (state->log, "Rate doesn't match (requested %iHz, get %iHz)", info->rate, rrate);
    if (flags & SPA_PORT_FORMAT_FLAG_NEAREST)
      info->rate = rrate;
    else
      return -EINVAL;
  }

  state->format = format;
  state->channels = info->channels;
  state->rate = info->rate;
  state->frame_size = info->channels * 2;

#if 0
  period_size = props->period_size;
  periods = props->periods;

  spa_log_info (state->log, "trying period frames %zd and periods %u", period_size, periods);

  dir = 0;
  CHECK (snd_pcm_hw_params_set_period_size_near (hndl, params, &period_size, &dir), "set_period_size_near");
  state->period_frames = period_size;

  dir = 0;
  CHECK (snd_pcm_hw_params_set_periods_near (hndl, params, &periods, &dir), "set_periods_near");
  state->buffer_frames = periods * state->period_frames;

  CHECK (snd_pcm_hw_params_set_buffer_size (hndl, params, state->buffer_frames), "set_buffer_size");
#else
  CHECK (snd_pcm_hw_params_get_buffer_size_max (params, &state->buffer_frames), "get_buffer_size_max");

  CHECK (snd_pcm_hw_params_set_buffer_size_near (hndl, params, &state->buffer_frames), "set_buffer_size_near");

  dir = 0;
  period_size = state->buffer_frames;
  CHECK (snd_pcm_hw_params_set_period_size_near (hndl, params, &period_size, &dir), "set_period_size_near");
  state->period_frames = period_size;
  periods = state->buffer_frames / state->period_frames;
#endif

  spa_log_info (state->log, "buffer frames %zd, period frames %zd, periods %u",
      state->buffer_frames, state->period_frames, periods);

  /* write the parameters to device */
  CHECK (snd_pcm_hw_params (hndl, params), "set_hw_params");

  return 0;
}

static int
set_swparams (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err = 0;
  snd_pcm_sw_params_t *params;
  snd_pcm_uframes_t boundary;

  SpaALSAProps *props = &state->props;

  snd_pcm_sw_params_alloca (&params);

  /* get the current params */
  CHECK (snd_pcm_sw_params_current (hndl, params), "sw_params_current");

  CHECK (snd_pcm_sw_params_set_tstamp_mode (hndl, params, SND_PCM_TSTAMP_ENABLE), "sw_params_set_tstamp_mode");

  /* start the transfer */
  CHECK (snd_pcm_sw_params_set_start_threshold (hndl, params, LONG_MAX), "set_start_threshold");
#if 1
  CHECK (snd_pcm_sw_params_get_boundary (params, &boundary), "get_boundary");

  CHECK (snd_pcm_sw_params_set_stop_threshold (hndl, params, boundary), "set_stop_threshold");
#else
  CHECK (snd_pcm_sw_params_set_stop_threshold (hndl, params, -1), "set_stop_threshold");
#endif

//  CHECK (snd_pcm_sw_params_set_silence_threshold (hndl, params, 0U), "set_silence_threshold");

  /* enable period events when requested */
//  CHECK (snd_pcm_sw_params_set_period_event (hndl, params, props->period_event ? 1 : 0), "set_period_event");
  CHECK (snd_pcm_sw_params_set_period_event (hndl, params, 0), "set_period_event");

#if 1
  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
//  CHECK (snd_pcm_sw_params_set_avail_min (hndl, params, state->period_frames), "set_avail_min");
#else
  CHECK (snd_pcm_sw_params_set_avail_min (hndl, params, 0), "set_avail_min");
#endif

  /* write the parameters to the playback device */
  CHECK (snd_pcm_sw_params (hndl, params), "sw_params");

  return 0;
}

/*
 *   Underrun and suspend recovery
 */
static int
xrun_recovery (SpaALSAState *state, snd_pcm_t *hndl, int err)
{
  snd_pcm_status_t *status;

  snd_pcm_status_alloca (&status);

  if ((err = snd_pcm_status (hndl, status)) < 0) {
    spa_log_error (state->log, "snd_pcm_status error: %s", snd_strerror (err));
  }

  if (snd_pcm_status_get_state (status) == SND_PCM_STATE_SUSPENDED) {
    spa_log_warn (state->log, "SUSPENDED, trying to resume");

    if ((err = snd_pcm_prepare (hndl)) < 0) {
      spa_log_error (state->log, "snd_pcm_prepare error: %s", snd_strerror (err));
    }
  }
  if (snd_pcm_status_get_state (status) == SND_PCM_STATE_XRUN) {
    spa_log_warn (state->log, "XRUN");
  }

  if (spa_alsa_pause (state, true) != SPA_RESULT_OK)
    return -1;
  if (spa_alsa_start (state, true) != SPA_RESULT_OK)
    return -1;

  return err;
}

static snd_pcm_uframes_t
pull_frames_queue (SpaALSAState *state,
                   const snd_pcm_channel_area_t *my_areas,
                   snd_pcm_uframes_t offset,
                   snd_pcm_uframes_t frames)
{
  snd_pcm_uframes_t total_frames = 0, to_write = frames;

  if (spa_list_is_empty (&state->ready)) {
    SpaEvent event = SPA_EVENT_INIT (state->type.event_node.NeedInput);
    state->event_cb (&state->node, &event, state->user_data);
  }
  while (!spa_list_is_empty (&state->ready) && to_write > 0) {
    uint8_t *src, *dst;
    size_t n_bytes, n_frames, size;
    off_t offs;
    SpaALSABuffer *b;

    b = spa_list_first (&state->ready, SpaALSABuffer, link);

    offs = SPA_MIN (b->outbuf->datas[0].chunk->offset, b->outbuf->datas[0].maxsize);
    src = SPA_MEMBER (b->outbuf->datas[0].data, offs, uint8_t);
    size = SPA_MIN (b->outbuf->datas[0].chunk->size, b->outbuf->datas[0].maxsize - offs);

    src = SPA_MEMBER (src, state->ready_offset, uint8_t);
    dst = SPA_MEMBER (my_areas[0].addr, offset * state->frame_size, uint8_t);
    n_bytes = SPA_MIN (size - state->ready_offset, to_write * state->frame_size);
    n_frames = SPA_MIN (to_write, n_bytes / state->frame_size);

    memcpy (dst, src, n_bytes);

    state->ready_offset += n_bytes;
    if (state->ready_offset >= size) {
      SpaEventNodeReuseBuffer rb = SPA_EVENT_NODE_REUSE_BUFFER_INIT (state->type.event_node.ReuseBuffer,
                                                                     0, b->outbuf->id);

      spa_list_remove (&b->link);
      b->outstanding = true;

      state->event_cb (&state->node, (SpaEvent *)&rb, state->user_data);

      state->ready_offset = 0;
    }
    total_frames += n_frames;
    to_write -= n_frames;
  }
  if (total_frames == 0) {
    total_frames = state->threshold;
    spa_log_warn (state->log, "underrun, want %zd frames", total_frames);
    snd_pcm_areas_silence (my_areas, offset, state->channels, total_frames, state->format);
  }
  //spa_log_warn (state->log, "written %zd frames", total_frames);
  return total_frames;
}

static snd_pcm_uframes_t
pull_frames_ringbuffer (SpaALSAState *state,
                        const snd_pcm_channel_area_t *my_areas,
                        snd_pcm_uframes_t offset,
                        snd_pcm_uframes_t frames)
{
  SpaRingbufferArea areas[2];
  size_t size, avail;
  SpaALSABuffer *b;
  uint8_t *src, *dst;

  b = state->ringbuffer;

  src = b->outbuf->datas[0].data;
  dst = SPA_MEMBER (my_areas[0].addr, offset * state->frame_size, uint8_t);

  avail = spa_ringbuffer_get_read_areas (&b->rb->ringbuffer, areas);
  size = SPA_MIN (avail, frames * state->frame_size);

  spa_log_debug (state->log, "%u %u %u %u %zd %zd",
      areas[0].offset, areas[0].len,
      areas[1].offset, areas[1].len, offset, size);

  if (size > 0) {
    spa_ringbuffer_read_data (&b->rb->ringbuffer,
                              src,
                              areas,
                              dst,
                              size);
    spa_ringbuffer_read_advance (&b->rb->ringbuffer, size);
    frames = size / state->frame_size;
  } else {
    spa_log_warn (state->log, "underrun");
    snd_pcm_areas_silence (my_areas, offset, state->channels, frames, state->format);
  }

  b->outstanding = true;
  {
    SpaEventNodeReuseBuffer rb = SPA_EVENT_NODE_REUSE_BUFFER_INIT (state->type.event_node.ReuseBuffer,
                                                                   0, b->outbuf->id);
    state->event_cb (&state->node, (SpaEvent*)&rb, state->user_data);
  }

  return frames;
}
static int
mmap_write (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err;
  snd_pcm_sframes_t avail;
  snd_pcm_uframes_t offset, frames, size;
  const snd_pcm_channel_area_t *my_areas;

#if 0
  snd_pcm_status_t *status;

  snd_pcm_status_alloca (&status);

  if ((err = snd_pcm_status (hndl, status)) < 0) {
    spa_log_error (state->log, "snd_pcm_status error: %s", snd_strerror (err));
    return -1;
  }

  avail = snd_pcm_status_get_avail (status);
#else
  if ((avail = snd_pcm_avail_update (hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_avail_update error: %s", snd_strerror (avail));
    return -1;
  }
#endif

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(err));
      return -1;
    }
    if (frames < state->period_frames)
      break;
    else
      frames = state->period_frames;

    if (state->ringbuffer)
      frames = pull_frames_ringbuffer (state, my_areas, offset, frames);
    else
      frames = pull_frames_queue (state, my_areas, offset, frames);

    if ((err = snd_pcm_mmap_commit (hndl, offset, frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(err));
      if (err != -EPIPE && err != -ESTRPIPE)
        return -1;
    }
    size -= frames;
  }
  return 0;
}

static int
mmap_read (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err;
  snd_pcm_sframes_t avail;
  snd_pcm_uframes_t offset, frames, size;
  snd_pcm_status_t *status;
  const snd_pcm_channel_area_t *my_areas;
  SpaALSABuffer *b;
  snd_htimestamp_t htstamp = { 0, 0 };
  int64_t now;
  uint8_t *dest = NULL;
  size_t destsize;

  snd_pcm_status_alloca(&status);

  if ((err = snd_pcm_status (hndl, status)) < 0) {
    spa_log_error (state->log, "snd_pcm_status error: %s", snd_strerror(err));
    return err;
  }

  avail = snd_pcm_status_get_avail (status);
  snd_pcm_status_get_htstamp (status, &htstamp);
  now = (int64_t)htstamp.tv_sec * SPA_NSEC_PER_SEC + (int64_t)htstamp.tv_nsec;

  state->last_ticks = state->sample_count * SPA_USEC_PER_SEC / state->rate;
  state->last_monotonic = now;

  if (spa_list_is_empty (&state->free)) {
    b = NULL;
    spa_log_warn (state->log, "no more buffers");
  } else {
    b = spa_list_first (&state->free, SpaALSABuffer, link);
    spa_list_remove (&b->link);

    dest = b->outbuf->datas[0].data;
    destsize = b->outbuf->datas[0].maxsize;

    if (b->h) {
      b->h->seq = state->sample_count;
      b->h->pts = state->last_monotonic;
      b->h->dts_offset = 0;
    }
    avail = SPA_MIN (avail, destsize / state->frame_size);
  }

  state->sample_count += avail;

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_begin error: %s", snd_strerror (err));
      return -1;
    }

    if (b) {
      size_t n_bytes = frames * state->frame_size;

      memcpy (dest,
              (uint8_t *)my_areas[0].addr + (offset * state->frame_size),
              n_bytes);
      dest += n_bytes;
    }

    if ((err = snd_pcm_mmap_commit (hndl, offset, frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(err));
      return -1;
    }
    size -= frames;
  }

  if (b) {
    SpaData *d;
    SpaPortOutput *output;

    d = b->outbuf->datas;
    d[0].chunk->offset = 0;
    d[0].chunk->size = avail * state->frame_size;
    d[0].chunk->stride = 0;

    if ((output = state->io)) {
      b->outstanding = true;
      output->buffer_id = b->outbuf->id;
      output->status = SPA_RESULT_OK;
    }
    {
      SpaEvent event = SPA_EVENT_INIT (state->type.event_node.HaveOutput);
      state->event_cb (&state->node, &event, state->user_data);
    }
  }
  return 0;
}

static inline short
spa_io_to_poll (SpaIO mask)
{
  short events = 0;

  if (mask & SPA_IO_IN)
    events |= POLLIN;
  if (mask & SPA_IO_OUT)
    events |= POLLOUT;
  if (mask & SPA_IO_ERR)
    events |= POLLERR;
  if (mask & SPA_IO_HUP)
    events |= POLLHUP;

  return events;
}

static inline SpaIO
spa_poll_to_io (short events)
{
  SpaIO mask = 0;

  if (events & POLLIN)
    mask |= SPA_IO_IN;
  if (events & POLLOUT)
    mask |= SPA_IO_OUT;
  if (events & POLLERR)
    mask |= SPA_IO_ERR;
  if (events & POLLHUP)
    mask |= SPA_IO_HUP;

  return mask;
}

static void
alsa_on_fd_events (SpaSource *source)
{
  SpaALSAState *state = source->data;
  snd_pcm_t *hndl = state->hndl;
  int err, i;
  unsigned short revents = 0;

  for (i = 0; i < state->n_fds; i++) {
    state->fds[i].revents = spa_io_to_poll (state->sources[i].rmask);
    state->sources[i].rmask = 0;
  }

  snd_pcm_poll_descriptors_revents (hndl,
                                    state->fds,
                                    state->n_fds,
                                    &revents);
  if (revents & POLLERR) {
    if ((err = xrun_recovery (state, hndl, err)) < 0) {
      spa_log_error (state->log, "error: %s", snd_strerror (err));
    }
  }

  if (state->stream == SND_PCM_STREAM_CAPTURE) {
    if (!(revents & POLLIN))
      return;

    mmap_read (state);
  } else {
    if (!(revents & POLLOUT))
      return;

    mmap_write (state);
  }
}

static int
alsa_try_resume (SpaALSAState *state)
{
  int res;

  while ((res = snd_pcm_resume (state->hndl)) == -EAGAIN)
    usleep (250000);
  if (res < 0) {
    spa_log_error (state->log, "suspended, failed to resume %s", snd_strerror(res));
    res = snd_pcm_prepare (state->hndl);
    if (res < 0)
      spa_log_error (state->log, "suspended, failed to prepare %s", snd_strerror(res));
  }
  return res;
}

static inline void
calc_timeout (size_t frames,
              size_t cb_threshold,
              size_t rate,
              snd_htimestamp_t *now,
              struct timespec *ts)
{
  size_t to_play_usec;

  ts->tv_sec = now->tv_sec;
  /* adjust sleep time to target our callback threshold */
  if (frames > cb_threshold)
    to_play_usec = (frames - cb_threshold) * 1000000 / rate;
  else
    to_play_usec = 0;

  ts->tv_nsec = to_play_usec * 1000 + now->tv_nsec;

  while (ts->tv_nsec > 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

static void
alsa_on_timeout_event (SpaSource *source)
{
  uint64_t exp;
  int res;
  SpaALSAState *state = source->data;
  snd_pcm_t *hndl = state->hndl;
  snd_pcm_sframes_t avail, delay;
  struct itimerspec ts;
  snd_pcm_uframes_t total_written = 0, filled;
  const snd_pcm_channel_area_t *my_areas;
  snd_pcm_status_t *status;
  snd_htimestamp_t htstamp;

  read (state->timerfd, &exp, sizeof (uint64_t));

  snd_pcm_status_alloca(&status);

  if ((res = snd_pcm_status (hndl, status)) < 0) {
    spa_log_error (state->log, "snd_pcm_status error: %s", snd_strerror (res));
    return;
  }

  avail = snd_pcm_status_get_avail (status);
  delay = snd_pcm_status_get_delay (status);
  snd_pcm_status_get_htstamp (status, &htstamp);

  state->last_ticks = state->sample_count - delay;
  state->last_monotonic = (int64_t)htstamp.tv_sec * SPA_NSEC_PER_SEC + (int64_t)htstamp.tv_nsec;

  if (avail > state->buffer_frames)
    avail = state->buffer_frames;

  filled = state->buffer_frames - avail;
  if (filled > state->threshold + 10) {
    if (snd_pcm_state (hndl) == SND_PCM_STATE_SUSPENDED) {
      spa_log_error (state->log, "suspended: try resume");
      if ((res = alsa_try_resume (state)) < 0)
        return;
    }
  }
  else {
    snd_pcm_uframes_t to_write = state->buffer_frames - filled;

    while (total_written < to_write) {
      snd_pcm_uframes_t written, frames, offset;

      frames = to_write - total_written;
      if ((res = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
        spa_log_error (state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(res));
        return;
      }

      if (state->ringbuffer)
        written = pull_frames_ringbuffer (state, my_areas, offset, frames);
      else
        written = pull_frames_queue (state, my_areas, offset, frames);

      if (written < frames)
        to_write = 0;

      if ((res = snd_pcm_mmap_commit (hndl, offset, written)) < 0) {
        spa_log_error (state->log, "snd_pcm_mmap_commit error: %s", snd_strerror(res));
        if (res != -EPIPE && res != -ESTRPIPE)
          return;
      }
      total_written += written;
    }
    state->sample_count += total_written;
  }
  if (!state->alsa_started && total_written > 0) {
    spa_log_trace (state->log, "snd_pcm_start");
    if ((res = snd_pcm_start (state->hndl)) < 0) {
      spa_log_error (state->log, "snd_pcm_start: %s", snd_strerror (res));
      return;
    }
    state->alsa_started = true;
  }

  calc_timeout (total_written + filled, state->threshold, state->rate, &htstamp, &ts.it_value);

//  printf ("timeout %ld %ld %ld %ld\n", total_written, filled, ts.it_value.tv_sec, ts.it_value.tv_nsec);

  ts.it_interval.tv_sec = 0;
  ts.it_interval.tv_nsec = 0;
  timerfd_settime (state->timerfd, TFD_TIMER_ABSTIME, &ts, NULL);
}

SpaResult
spa_alsa_start (SpaALSAState *state, bool xrun_recover)
{
  int err, i;

  if (state->started)
    return SPA_RESULT_OK;

  CHECK (set_swparams (state), "swparams");
  if (!xrun_recover)
    snd_pcm_dump (state->hndl, state->output);

  if ((err = snd_pcm_prepare (state->hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_prepare error: %s", snd_strerror (err));
    return SPA_RESULT_ERROR;
  }

  for (i = 0; i < state->n_fds; i++)
    spa_loop_remove_source (state->data_loop, &state->sources[i]);

#if 0
  if ((state->n_fds = snd_pcm_poll_descriptors_count (state->hndl)) <= 0) {
    spa_log_error (state->log, "Invalid poll descriptors count %d", state->n_fds);
    return SPA_RESULT_ERROR;
  }

  if ((err = snd_pcm_poll_descriptors (state->hndl, state->fds, state->n_fds)) < 0) {
    spa_log_error (state->log, "snd_pcm_poll_descriptors: %s", snd_strerror(err));
    return SPA_RESULT_ERROR;
  }

  for (i = 0; i < state->n_fds; i++) {
    state->sources[i].func = alsa_on_fd_events;
    state->sources[i].data = state;
    state->sources[i].fd = state->fds[i].fd;
    state->sources[i].mask = spa_poll_to_io (state->fds[i].events);
    state->sources[i].rmask = 0;
    state->fds[i].revents  = 0;
    spa_loop_add_source (state->data_loop, &state->sources[i]);
  }
#else
  state->n_fds = 1;
  state->sources[0].func = alsa_on_timeout_event;
  state->sources[0].data = state;
  state->sources[0].fd = state->timerfd;
  state->sources[0].mask = SPA_IO_IN;
  state->sources[0].rmask = 0;
  spa_loop_add_source (state->data_loop, &state->sources[0]);
#endif

  state->threshold = 40;
  state->alsa_started = false;

  if (state->stream == SND_PCM_STREAM_PLAYBACK) {
    alsa_on_timeout_event (&state->sources[0]);
  }

#if 0
  if ((err = snd_pcm_start (state->hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_start: %s", snd_strerror (err));
    return SPA_RESULT_ERROR;
  }
#endif

  state->started = true;

  return SPA_RESULT_OK;
}

SpaResult
spa_alsa_pause (SpaALSAState *state, bool xrun_recover)
{
  int err, i;

  if (!state->started)
    return SPA_RESULT_OK;

  for (i = 0; i < state->n_fds; i++)
    spa_loop_remove_source (state->data_loop, &state->sources[i]);
  state->n_fds = 0;

  if ((err = snd_pcm_drop (state->hndl)) < 0)
    spa_log_error (state->log, "snd_pcm_drop %s", snd_strerror (err));

  state->started = false;

  return SPA_RESULT_OK;
}
