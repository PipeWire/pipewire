#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>

#include <lib/debug.h>
#include "alsa-utils.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error (state->log, msg ": %s", snd_strerror(err)); return err; }

static void alsa_on_fd_events (SpaSource *source);

static int
spa_alsa_open (SpaALSAState *state)
{
  int err;
  SpaALSAProps *props = &state->props[1];

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

  state->opened = false;

  return err;
}

static snd_pcm_format_t
spa_alsa_format_to_alsa (SpaAudioFormat format)
{
  switch (format) {
    case SPA_AUDIO_FORMAT_S8:
      return SND_PCM_FORMAT_S8;
    case SPA_AUDIO_FORMAT_U8:
      return SND_PCM_FORMAT_U8;
    /* 16 bit */
    case SPA_AUDIO_FORMAT_S16LE:
      return SND_PCM_FORMAT_S16_LE;
    case SPA_AUDIO_FORMAT_S16BE:
      return SND_PCM_FORMAT_S16_BE;
    case SPA_AUDIO_FORMAT_U16LE:
      return SND_PCM_FORMAT_U16_LE;
    case SPA_AUDIO_FORMAT_U16BE:
      return SND_PCM_FORMAT_U16_BE;
    /* 24 bit in low 3 bytes of 32 bits */
    case SPA_AUDIO_FORMAT_S24_32LE:
      return SND_PCM_FORMAT_S24_LE;
    case SPA_AUDIO_FORMAT_S24_32BE:
      return SND_PCM_FORMAT_S24_BE;
    case SPA_AUDIO_FORMAT_U24_32LE:
      return SND_PCM_FORMAT_U24_LE;
    case SPA_AUDIO_FORMAT_U24_32BE:
      return SND_PCM_FORMAT_U24_BE;
    /* 24 bit in 3 bytes */
    case SPA_AUDIO_FORMAT_S24LE:
      return SND_PCM_FORMAT_S24_3LE;
    case SPA_AUDIO_FORMAT_S24BE:
      return SND_PCM_FORMAT_S24_3BE;
    case SPA_AUDIO_FORMAT_U24LE:
      return SND_PCM_FORMAT_U24_3LE;
    case SPA_AUDIO_FORMAT_U24BE:
      return SND_PCM_FORMAT_U24_3BE;
    /* 32 bit */
    case SPA_AUDIO_FORMAT_S32LE:
      return SND_PCM_FORMAT_S32_LE;
    case SPA_AUDIO_FORMAT_S32BE:
      return SND_PCM_FORMAT_S32_BE;
    case SPA_AUDIO_FORMAT_U32LE:
      return SND_PCM_FORMAT_U32_LE;
    case SPA_AUDIO_FORMAT_U32BE:
      return SND_PCM_FORMAT_U32_BE;
    default:
      break;
  }

  return SND_PCM_FORMAT_UNKNOWN;
}

int
spa_alsa_set_format (SpaALSAState *state, SpaFormatAudio *fmt, SpaPortFormatFlags flags)
{
  unsigned int rrate, rchannels;
  snd_pcm_uframes_t size;
  int err, dir;
  snd_pcm_hw_params_t *params;
  snd_pcm_format_t format;
  SpaAudioInfoRaw *info = &fmt->info.raw;
  snd_pcm_t *hndl;
  unsigned int buffer_time;
  unsigned int period_time;
  SpaALSAProps *props = &state->props[1];

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

  /* set the sample format */
  format = spa_alsa_format_to_alsa (info->format);
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

  /* set the buffer time */
  buffer_time = props->buffer_time;
  CHECK (snd_pcm_hw_params_set_buffer_time_near (hndl, params, &buffer_time, &dir), "set_buffer_time_near");
  CHECK (snd_pcm_hw_params_get_buffer_size (params, &size), "get_buffer_size");
  state->buffer_frames = size;

  /* set the period time */
  period_time = props->period_time;
  CHECK (snd_pcm_hw_params_set_period_time_near (hndl, params, &period_time, &dir), "set_period_time_near");
  CHECK (snd_pcm_hw_params_get_period_size (params, &size, &dir), "get_period_size");
  state->period_frames = size;

  spa_log_info (state->log, "buffer frames %zd, period frames %zd", state->buffer_frames, state->period_frames);

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
  SpaALSAProps *props = &state->props[1];

  snd_pcm_sw_params_alloca (&params);

  /* get the current params */
  CHECK (snd_pcm_sw_params_current (hndl, params), "sw_params_current");

  CHECK (snd_pcm_sw_params_set_tstamp_mode (hndl, params, SND_PCM_TSTAMP_ENABLE), "sw_params_set_tstamp_mode");

  /* start the transfer */
  CHECK (snd_pcm_sw_params_set_start_threshold (hndl, params, 0U), "set_start_threshold");
  CHECK (snd_pcm_sw_params_set_stop_threshold (hndl, params,
         (state->buffer_frames / state->period_frames) * state->period_frames), "set_stop_threshold");
//  CHECK (snd_pcm_sw_params_set_stop_threshold (hndl, params, -1), "set_stop_threshold");

  CHECK (snd_pcm_sw_params_set_silence_threshold (hndl, params, 0U), "set_silence_threshold");

#if 1
  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
  CHECK (snd_pcm_sw_params_set_avail_min (hndl, params,
        props->period_event ? state->buffer_frames : state->period_frames), "set_avail_min");

  /* enable period events when requested */
  if (props->period_event) {
    CHECK (snd_pcm_sw_params_set_period_event (hndl, params, 1), "set_period_event");
  }
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
  if (spa_list_is_empty (&state->ready)) {
    SpaNodeEventNeedInput ni;

    ni.event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
    ni.event.size = sizeof (ni);
    ni.port_id = 0;
    state->event_cb (&state->node, &ni.event, state->user_data);
  }
  if (!spa_list_is_empty (&state->ready)) {
    uint8_t *src, *dst;
    size_t n_bytes, size;
    off_t offs;
    SpaALSABuffer *b;

    b = spa_list_first (&state->ready, SpaALSABuffer, link);

    offs = SPA_MIN (b->outbuf->datas[0].chunk->offset, b->outbuf->datas[0].maxsize);
    src = SPA_MEMBER (b->outbuf->datas[0].data, offs, uint8_t);
    size = SPA_MIN (b->outbuf->datas[0].chunk->size, b->outbuf->datas[0].maxsize - offs);

    src = SPA_MEMBER (src, state->ready_offset, uint8_t);
    dst = SPA_MEMBER (my_areas[0].addr, offset * state->frame_size, uint8_t);
    n_bytes = SPA_MIN (size - state->ready_offset, frames * state->frame_size);
    frames = SPA_MIN (frames, n_bytes / state->frame_size);

    memcpy (dst, src, n_bytes);

    state->ready_offset += n_bytes;
    if (state->ready_offset >= size) {
      SpaNodeEventReuseBuffer rb;

      spa_list_remove (&b->link);
      b->outstanding = true;

      rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
      rb.event.size = sizeof (rb);
      rb.port_id = 0;
      rb.buffer_id = b->outbuf->id;
      state->event_cb (&state->node, &rb.event, state->user_data);

      state->ready_offset = 0;
    }
  } else {
    spa_log_warn (state->log, "underrun, want %zd frames", frames);
    snd_pcm_areas_silence (my_areas, offset, state->channels, frames, state->format);
  }
  return frames;
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
  SpaNodeEventReuseBuffer rb;

  b = state->ringbuffer;

  src = b->outbuf->datas[0].data;
  dst = SPA_MEMBER (my_areas[0].addr, offset * state->frame_size, uint8_t);

  avail = spa_ringbuffer_get_read_areas (&b->rb->ringbuffer, areas);
  size = SPA_MIN (avail, frames * state->frame_size);

  spa_log_debug (state->log, "%zd %zd %zd %zd %zd %zd",
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
  rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  rb.event.size = sizeof (rb);
  rb.port_id = 0;
  rb.buffer_id = b->outbuf->id;
  state->event_cb (&state->node, &rb.event, state->user_data);

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
  SpaNodeEventNeedInput ni;

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

  ni.event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
  ni.event.size = sizeof (ni);
  ni.port_id = 0;
  state->event_cb (&state->node, &ni.event, state->user_data);

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_begin error: %s", snd_strerror(err));
      return -1;
    }

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
    SpaNodeEventHaveOutput ho;
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
    ho.event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    ho.event.size = sizeof (ho);
    ho.port_id = 0;
    state->event_cb (&state->node, &ho.event, state->user_data);
  }
  return 0;
}

static void
alsa_on_fd_events (SpaSource *source)
{
  SpaALSAState *state = source->data;
  snd_pcm_t *hndl = state->hndl;
  int err;
  unsigned short revents = 0;

#if 0
  snd_pcm_poll_descriptors_revents (hndl,
                                    state->fds,
                                    state->n_fds,
                                    &revents);

  spa_log_debug (state->log, "revents: %d %d", revents, state->n_fds);
#endif
  revents = source->rmask;

  if (revents & SPA_IO_ERR) {
    if ((err = xrun_recovery (state, hndl, err)) < 0) {
      spa_log_error (state->log, "error: %s", snd_strerror (err));
      return;
    }
  }

  if (state->stream == SND_PCM_STREAM_CAPTURE) {
    if (!(revents & SPA_IO_IN))
      return;

    mmap_read (state);
  } else {
    if (!(revents & SPA_IO_OUT))
      return;

    mmap_write (state);
  }
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
    state->sources[i].mask = 0;
    if (state->fds[i].events & POLLIN)
      state->sources[i].mask |= SPA_IO_IN;
    if (state->fds[i].events & POLLOUT)
      state->sources[i].mask |= SPA_IO_OUT;
    if (state->fds[i].events & POLLERR)
      state->sources[i].mask |= SPA_IO_ERR;
    if (state->fds[i].events & POLLHUP)
      state->sources[i].mask |= SPA_IO_HUP;
    spa_loop_add_source (state->data_loop, &state->sources[i]);
  }

  if (state->stream == SND_PCM_STREAM_PLAYBACK) {
    mmap_write (state);
  }

  if ((err = snd_pcm_start (state->hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_start: %s", snd_strerror (err));
    return SPA_RESULT_ERROR;
  }

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
