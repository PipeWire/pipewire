#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>

#include "alsa-utils.h"

#define CHECK(s,msg) if ((err = (s)) < 0) { spa_log_error (state->log, msg ": %s\n", snd_strerror(err)); return err; }

static int alsa_on_fd_events (SpaPollNotifyData *data);

static int
spa_alsa_open (SpaALSAState *state)
{
  int err;
  SpaALSAProps *props = &state->props[1];

  if (state->opened)
    return 0;

  CHECK (snd_output_stdio_attach (&state->output, stderr, 0), "attach failed");

  spa_log_info (state->log, "ALSA device open '%s'\n", props->device);
  CHECK (snd_pcm_open (&state->hndl,
                       props->device,
                       state->stream,
                       SND_PCM_NONBLOCK |
                       SND_PCM_NO_AUTO_RESAMPLE |
                       SND_PCM_NO_AUTO_CHANNELS |
                       SND_PCM_NO_AUTO_FORMAT), "open failed");


  state->poll.id = 0;
  state->poll.enabled = false;
  state->poll.fds = state->fds;
  state->poll.n_fds = 0;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = alsa_on_fd_events;
  state->poll.user_data = state;
  spa_poll_add_item (state->data_loop, &state->poll);

  state->opened = true;

  return 0;
}

int
spa_alsa_close (SpaALSAState *state)
{
  int err = 0;

  if (!state->opened)
    return 0;

  spa_poll_remove_item (state->data_loop, &state->poll);

  spa_log_info (state->log, "Device closing\n");
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
  spa_log_info (state->log, "Stream parameters are %iHz, %s, %i channels\n", info->rate, snd_pcm_format_name(format), info->channels);
  CHECK (snd_pcm_hw_params_set_format (hndl, params, format), "set_format");

  /* set the count of channels */
  rchannels = info->channels;
  CHECK (snd_pcm_hw_params_set_channels_near (hndl, params, &rchannels), "set_channels");
  if (rchannels != info->channels) {
    spa_log_info (state->log, "Channels doesn't match (requested %u, get %u\n", info->channels, rchannels);
    if (flags & SPA_PORT_FORMAT_FLAG_NEAREST)
      info->channels = rchannels;
    else
      return -EINVAL;
  }

  /* set the stream rate */
  rrate = info->rate;
  CHECK (snd_pcm_hw_params_set_rate_near (hndl, params, &rrate, 0), "set_rate_near");
  if (rrate != info->rate) {
    spa_log_info (state->log, "Rate doesn't match (requested %iHz, get %iHz)\n", info->rate, rrate);
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

  /* start the transfer when the buffer is almost full: */
  /* (buffer_frames / avail_min) * avail_min */
  CHECK (snd_pcm_sw_params_set_start_threshold (hndl, params,
        (state->buffer_frames / state->period_frames) * state->period_frames), "set_start_threshold");

  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
  CHECK (snd_pcm_sw_params_set_avail_min (hndl, params,
        props->period_event ? state->buffer_frames : state->period_frames), "set_avail_min");
  /* enable period events when requested */
  if (props->period_event) {
    CHECK (snd_pcm_sw_params_set_period_event (hndl, params, 1), "set_period_event");
  }
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
    spa_log_error (state->log, "snd_pcm_status error: %s\n", snd_strerror (err));
  }

  if (snd_pcm_status_get_state (status) == SND_PCM_STATE_SUSPENDED) {
    spa_log_info (state->log, "SUSPENDED, trying to resume\n");

    if ((err = snd_pcm_prepare (hndl)) < 0) {
      spa_log_error (state->log, "snd_pcm_prepare error: %s\n", snd_strerror (err));
    }
  }
  if (snd_pcm_status_get_state (status) == SND_PCM_STATE_XRUN) {
    spa_log_info (state->log, "XRUN\n");
  }

  if (spa_alsa_pause (state, true) != SPA_RESULT_OK)
    return -1;
  if (spa_alsa_start (state, true) != SPA_RESULT_OK)
    return -1;

  return err;
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
  SpaALSABuffer *b;
  snd_pcm_status_t *status;

  snd_pcm_status_alloca (&status);

  if ((err = snd_pcm_status (hndl, status)) < 0) {
    spa_log_error (state->log, "snd_pcm_status error: %s\n", snd_strerror (err));
    return -1;
  }

  avail = snd_pcm_status_get_avail (status);

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_begin error: %s\n", snd_strerror(err));
      return -1;
    }

    ni.event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
    ni.event.size = sizeof (ni);
    ni.port_id = 0;
    state->event_cb (&state->node, &ni.event, state->user_data);

    SPA_QUEUE_PEEK_HEAD (&state->ready, SpaALSABuffer, b);

    if (b) {
      uint8_t *src;
      size_t n_bytes;

      src = SPA_MEMBER (b->outbuf->datas[0].data, b->outbuf->datas[0].offset + state->ready_offset, void);
      n_bytes = SPA_MIN (b->outbuf->datas[0].size - state->ready_offset, frames * state->frame_size);
      frames = SPA_MIN (frames, n_bytes / state->frame_size);

      memcpy ((uint8_t *)my_areas[0].addr + (offset * state->frame_size),
              src,
              n_bytes);

      state->ready_offset += n_bytes;
      if (state->ready_offset >= b->outbuf->datas[0].size) {
        SpaNodeEventReuseBuffer rb;

        SPA_QUEUE_POP_HEAD (&state->ready, SpaALSABuffer, next, b);
        b->outstanding = true;

        rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
        rb.event.size = sizeof (rb);
        rb.port_id = 0;
        rb.buffer_id = b->outbuf->id;
        state->event_cb (&state->node, &rb.event, state->user_data);

        state->ready_offset = 0;
      }
    } else {
      spa_log_warn (state->log, "underrun\n");
      snd_pcm_areas_silence (my_areas, offset, state->channels, frames, state->format);
    }

    if ((err = snd_pcm_mmap_commit (hndl, offset, frames)) < 0) {
      spa_log_error (state->log, "snd_pcm_mmap_commit error: %s\n", snd_strerror(err));
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
    spa_log_error (state->log, "snd_pcm_status error: %s\n", snd_strerror(err));
    return err;
  }

  avail = snd_pcm_status_get_avail (status);
  snd_pcm_status_get_htstamp (status, &htstamp);
  now = (int64_t)htstamp.tv_sec * SPA_NSEC_PER_SEC + (int64_t)htstamp.tv_nsec;

  state->last_ticks = state->sample_count * SPA_USEC_PER_SEC / state->rate;
  state->last_monotonic = now;

  SPA_QUEUE_POP_HEAD (&state->free, SpaALSABuffer, next, b);
  if (b == NULL) {
    spa_log_warn (state->log, "no more buffers\n");
  } else {
    dest = SPA_MEMBER (b->outbuf->datas[0].data, b->outbuf->datas[0].offset, void);
    destsize = b->outbuf->datas[0].size;

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
      spa_log_error (state->log, "snd_pcm_mmap_begin error: %s\n", snd_strerror (err));
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
      spa_log_error (state->log, "snd_pcm_mmap_commit error: %s\n", snd_strerror(err));
      return -1;
    }
    size -= frames;
  }


  if (b) {
    SpaNodeEventHaveOutput ho;
    SpaData *d;

    d = b->outbuf->datas;
    d[0].size = avail * state->frame_size;

    b->next = NULL;
    SPA_QUEUE_PUSH_TAIL (&state->ready, SpaALSABuffer, next, b);

    ho.event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    ho.event.size = sizeof (ho);
    ho.port_id = 0;
    state->event_cb (&state->node, &ho.event, state->user_data);
  }
  return 0;
}

static int
alsa_on_fd_events (SpaPollNotifyData *data)
{
  SpaALSAState *state = data->user_data;
  snd_pcm_t *hndl = state->hndl;
  int err;
  unsigned short revents = 0;

  snd_pcm_poll_descriptors_revents (hndl,
                                    (struct pollfd *)data->fds,
                                    data->n_fds,
                                    &revents);
  if (revents & POLLERR) {
    if ((err = xrun_recovery (state, hndl, err)) < 0) {
      spa_log_error (state->log, "error: %s\n", snd_strerror (err));
      return -1;
    }
  }

  if (state->stream == SND_PCM_STREAM_CAPTURE) {
    if (!(revents & POLLIN))
      return 0;

    mmap_read (state);
  } else {
    if (!(revents & POLLOUT))
      return 0;

    mmap_write (state);
  }

  return 0;
}

SpaResult
spa_alsa_start (SpaALSAState *state, bool xrun_recover)
{
  int err;

  if (state->started)
    return SPA_RESULT_OK;

  CHECK (set_swparams (state), "swparams");
  if (!xrun_recover)
    snd_pcm_dump (state->hndl, state->output);

  if ((err = snd_pcm_prepare (state->hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_prepare error: %s\n", snd_strerror (err));
    return SPA_RESULT_ERROR;
  }

  if ((state->poll.n_fds = snd_pcm_poll_descriptors_count (state->hndl)) <= 0) {
    spa_log_error (state->log, "Invalid poll descriptors count %d\n", state->poll.n_fds);
    return SPA_RESULT_ERROR;
  }
  if ((err = snd_pcm_poll_descriptors (state->hndl, (struct pollfd *)state->fds, state->poll.n_fds)) < 0) {
    spa_log_error (state->log, "snd_pcm_poll_descriptors: %s\n", snd_strerror(err));
    return SPA_RESULT_ERROR;
  }

  if (!xrun_recover) {
    state->poll.enabled = true;
    spa_poll_update_item (state->data_loop, &state->poll);
  }

  if (state->stream == SND_PCM_STREAM_PLAYBACK) {
    mmap_write (state);
  }

  if ((err = snd_pcm_start (state->hndl)) < 0) {
    spa_log_error (state->log, "snd_pcm_start: %s\n", snd_strerror (err));
    return SPA_RESULT_ERROR;
  }

  state->started = true;

  return SPA_RESULT_OK;
}

SpaResult
spa_alsa_pause (SpaALSAState *state, bool xrun_recover)
{
  int err;

  if (!state->started)
    return SPA_RESULT_OK;

  if (!xrun_recover) {
    state->poll.enabled = false;
    spa_poll_update_item (state->data_loop, &state->poll);
  }

  if ((err = snd_pcm_drop (state->hndl)) < 0)
    spa_log_error (state->log, "snd_pcm_drop %s\n", snd_strerror (err));

  state->started = false;

  return SPA_RESULT_OK;
}
