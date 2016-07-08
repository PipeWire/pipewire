#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>

static int verbose = 0;					/* verbose flag */

#define CHECK(s,msg) if ((err = (s)) < 0) { printf (msg ": %s\n", snd_strerror(err)); return err; }

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

static int
set_hwparams (SpaALSASink *this)
{
  unsigned int rrate;
  snd_pcm_uframes_t size;
  int err, dir;
  snd_pcm_hw_params_t *params;
  snd_pcm_format_t format;
  SpaALSAState *state = &this->state;
  SpaAudioRawFormat *fmt = &this->current_format;
  SpaAudioRawInfo *info = &fmt->info;
  snd_pcm_t *handle = state->handle;
  unsigned int buffer_time;
  unsigned int period_time;
  SpaALSASinkProps *props = &this->props[1];

  snd_pcm_hw_params_alloca (&params);
  /* choose all parameters */
  CHECK (snd_pcm_hw_params_any (handle, params), "Broken configuration for playback: no configurations available");
  /* set hardware resampling */
  CHECK (snd_pcm_hw_params_set_rate_resample (handle, params, 0), "set_rate_resample");
  /* set the interleaved read/write format */
  CHECK (snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

  /* set the sample format */
  format = spa_alsa_format_to_alsa (info->format);
  printf ("Stream parameters are %iHz, %s, %i channels\n", info->rate, snd_pcm_format_name(format), info->channels);
  CHECK (snd_pcm_hw_params_set_format (handle, params, format), "set_format");
  /* set the count of channels */
  CHECK (snd_pcm_hw_params_set_channels (handle, params, info->channels), "set_channels");
  /* set the stream rate */
  rrate = info->rate;
  CHECK (snd_pcm_hw_params_set_rate_near (handle, params, &rrate, 0), "set_rate_near");
  if (rrate != info->rate) {
    printf("Rate doesn't match (requested %iHz, get %iHz)\n", info->rate, rrate);
    return -EINVAL;
  }
  /* set the buffer time */
  buffer_time = props->buffer_time;
  CHECK (snd_pcm_hw_params_set_buffer_time_near (handle, params, &buffer_time, &dir), "set_buffer_time_near");
  CHECK (snd_pcm_hw_params_get_buffer_size (params, &size), "get_buffer_size");
  state->buffer_size = size;

  /* set the period time */
  period_time = props->period_time;
  CHECK (snd_pcm_hw_params_set_period_time_near (handle, params, &period_time, &dir), "set_period_time_near");
  CHECK (snd_pcm_hw_params_get_period_size (params, &size, &dir), "get_period_size");
  state->period_size = size;

  /* write the parameters to device */
  CHECK (snd_pcm_hw_params (handle, params), "set_hw_params");

  return 0;
}

static int
set_swparams (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  snd_pcm_t *handle = state->handle;
  int err = 0;
  snd_pcm_sw_params_t *params;
  SpaALSASinkProps *props = &this->props[1];

  snd_pcm_sw_params_alloca (&params);

  /* get the current params */
  CHECK (snd_pcm_sw_params_current (handle, params), "sw_params_current");
  /* start the transfer when the buffer is almost full: */
  /* (buffer_size / avail_min) * avail_min */
  CHECK (snd_pcm_sw_params_set_start_threshold (handle, params,
        (state->buffer_size / state->period_size) * state->period_size), "set_start_threshold");

  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
  CHECK (snd_pcm_sw_params_set_avail_min (handle, params,
        props->period_event ? state->buffer_size : state->period_size), "set_avail_min");
  /* enable period events when requested */
  if (props->period_event) {
    CHECK (snd_pcm_sw_params_set_period_event (handle, params, 1), "set_period_event");
  }
  /* write the parameters to the playback device */
  CHECK (snd_pcm_sw_params (handle, params), "sw_params");

  return 0;
}

/*
 *   Underrun and suspend recovery
 */
static int
xrun_recovery (snd_pcm_t *handle, int err)
{
  if (verbose)
    printf("stream recovery\n");
  if (err == -EPIPE) {	/* under-run */
    err = snd_pcm_prepare(handle);
    if (err < 0)
      printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
    return 0;
  } else if (err == -ESTRPIPE) {
    while ((err = snd_pcm_resume(handle)) == -EAGAIN)
      sleep(1);	/* wait until the suspend flag is released */
    if (err < 0) {
      err = snd_pcm_prepare(handle);
      if (err < 0)
        printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
    }
    return 0;
  }
  return err;
}

static int
spa_alsa_open (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  int err;
  SpaALSASinkProps *props = &this->props[1];

  if (state->opened)
    return 0;

  CHECK (snd_output_stdio_attach (&state->output, stdout, 0), "attach failed");

  printf ("Playback device is '%s'\n", props->device);
  CHECK (snd_pcm_open (&state->handle,
                       props->device,
                       SND_PCM_STREAM_PLAYBACK,
                       SND_PCM_NONBLOCK |
                       SND_PCM_NO_AUTO_RESAMPLE |
                       SND_PCM_NO_AUTO_CHANNELS |
                       SND_PCM_NO_AUTO_FORMAT), "open failed");

  state->opened = true;

  return 0;
}

static void
pull_input (SpaALSASink *this, void *data, snd_pcm_uframes_t frames)
{
  SpaEvent event;
  ALSABuffer *buffer = &this->buffer;

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_PULL_INPUT;
  event.port_id = 0;
  event.size = frames * sizeof (uint16_t) * 2;
  event.data = buffer;

  buffer->buffer.refcount = 1;
  buffer->buffer.notify = NULL;
  buffer->buffer.size = frames * sizeof (uint16_t) * 2;
  buffer->buffer.n_metas = 1;
  buffer->buffer.metas = buffer->meta;
  buffer->buffer.n_datas = 1;
  buffer->buffer.datas = buffer->data;

  buffer->header.flags = 0;
  buffer->header.seq = 0;
  buffer->header.pts = 0;
  buffer->header.dts_offset = 0;

  buffer->meta[0].type = SPA_META_TYPE_HEADER;
  buffer->meta[0].data = &buffer->header;
  buffer->meta[0].size = sizeof (buffer->header);

  buffer->data[0].type = SPA_DATA_TYPE_MEMPTR;
  buffer->data[0].data = data;
  buffer->data[0].size = frames * sizeof (uint16_t) * 2;

  this->event_cb (&this->handle, &event,this->user_data);

  spa_buffer_unref ((SpaBuffer *)event.data);
}

static int
mmap_write (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  snd_pcm_t *handle = state->handle;
  int err;
  snd_pcm_sframes_t avail, commitres;
  snd_pcm_uframes_t offset, frames, size;
  const snd_pcm_channel_area_t *my_areas;

  if ((avail = snd_pcm_avail_update (handle)) < 0) {
    if ((err = xrun_recovery (handle, avail)) < 0) {
      printf ("Write error: %s\n", snd_strerror (err));
      return -1;
    }
  }

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (handle, &my_areas, &offset, &frames)) < 0) {
      if ((err = xrun_recovery(handle, err)) < 0) {
        printf("MMAP begin avail error: %s\n", snd_strerror(err));
        return -1;
      }
    }

    pull_input (this,
                (uint8_t *)my_areas[0].addr + (offset * sizeof (uint16_t) * 2),
                frames);

    if (this->input_buffer) {
      if (this->input_buffer != &this->buffer.buffer) {
        /* FIXME, copy input */
      }
      spa_buffer_unref (this->input_buffer);
      this->input_buffer = NULL;
    }

    commitres = snd_pcm_mmap_commit (handle, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((err = xrun_recovery (handle, commitres >= 0 ? -EPIPE : commitres)) < 0) {
        printf("MMAP commit error: %s\n", snd_strerror(err));
        return -1;
      }
    }
    size -= frames;
  }
  return 0;
}

static int
alsa_on_fd_events (SpaPollNotifyData *data)
{
  SpaALSASink *this = data->user_data;
  SpaALSAState *state = &this->state;
  snd_pcm_t *handle = state->handle;
  int err;
  unsigned short revents;

  snd_pcm_poll_descriptors_revents (handle,
                                    (struct pollfd *)data->fds,
                                    data->n_fds,
                                    &revents);
  if (revents & POLLERR) {
    if (snd_pcm_state (handle) == SND_PCM_STATE_XRUN ||
        snd_pcm_state (handle) == SND_PCM_STATE_SUSPENDED) {
      err = snd_pcm_state (handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
      if ((err = xrun_recovery (handle, err)) < 0) {
        printf ("Write error: %s\n", snd_strerror(err));
        return -1;
      }
    } else {
      printf("Wait for poll failed\n");
      return -1;
    }
  }
  if (!(revents & POLLOUT))
    return -1;

  mmap_write (this);

  return 0;
}

static int
spa_alsa_close (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  int err = 0;

  if (!state->opened)
    return 0;

  CHECK (snd_pcm_close (state->handle), "close failed");

  state->opened = false;

  return err;
}

static int
spa_alsa_start (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  int err;
  SpaEvent event;

  if (spa_alsa_open (this) < 0)
    return -1;

  CHECK (set_hwparams (this), "hwparams");
  CHECK (set_swparams (this), "swparams");

  snd_pcm_dump (state->handle, state->output);

  if ((state->poll.n_fds = snd_pcm_poll_descriptors_count (state->handle)) <= 0) {
    printf ("Invalid poll descriptors count\n");
    return state->poll.n_fds;
  }
  if ((err = snd_pcm_poll_descriptors (state->handle, (struct pollfd *)state->fds, state->poll.n_fds)) < 0) {
    printf ("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
    return err;
  }

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_ADD_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->poll.fds = state->fds;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = alsa_on_fd_events;
  state->poll.user_data = this;
  this->event_cb (&this->handle, &event, this->user_data);

  mmap_write (this);
  err = snd_pcm_start (state->handle);

  return err;
}

static int
spa_alsa_stop (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;
  SpaEvent event;

  snd_pcm_drop (state->handle);

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_REMOVE_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);
  this->event_cb (&this->handle, &event, this->user_data);

  spa_alsa_close (this);

  return 0;
}
