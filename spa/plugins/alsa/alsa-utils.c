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

/*
 *   Transfer method - direct write only
 */
static void *
direct_loop (void *user_data)
{
  SpaALSASink *this = user_data;
  SpaALSAState *state = &this->state;
  snd_pcm_t *handle = state->handle;
  const snd_pcm_channel_area_t *my_areas;
  snd_pcm_uframes_t offset, frames, size;
  snd_pcm_sframes_t avail, commitres;
  snd_pcm_state_t st;
  int err, first = 1;

  while (state->running) {
    st = snd_pcm_state(handle);
    if (st == SND_PCM_STATE_XRUN) {
      err = xrun_recovery(handle, -EPIPE);
      if (err < 0) {
        printf("XRUN recovery failed: %s\n", snd_strerror(err));
        return NULL;
      }
      first = 1;
    } else if (st == SND_PCM_STATE_SUSPENDED) {
      err = xrun_recovery(handle, -ESTRPIPE);
      if (err < 0) {
        printf("SUSPEND recovery failed: %s\n", snd_strerror(err));
        return NULL;
      }
    }
    avail = snd_pcm_avail_update(handle);
    if (avail < 0) {
      err = xrun_recovery(handle, avail);
      if (err < 0) {
        printf("avail update failed: %s\n", snd_strerror(err));
        return NULL;
      }
      first = 1;
      continue;
    }
    if (avail < state->period_size) {
      if (first) {
        first = 0;
        err = snd_pcm_start(handle);
        if (err < 0) {
          printf("Start error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
        }
      } else {
        err = snd_pcm_wait(handle, -1);
        if (err < 0) {
          if ((err = xrun_recovery(handle, err)) < 0) {
            printf("snd_pcm_wait error: %s\n", snd_strerror(err));
            exit(EXIT_FAILURE);
          }
          first = 1;
        }
      }
      continue;
    }
    size = state->period_size;
    while (size > 0) {
      frames = size;
      err = snd_pcm_mmap_begin(handle, &my_areas, &offset, &frames);
      if (err < 0) {
        if ((err = xrun_recovery(handle, err)) < 0) {
          printf("MMAP begin avail error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
        }
        first = 1;
      }

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
        buffer->data[0].data = (uint8_t *)my_areas[0].addr + (offset * sizeof (uint16_t) * 2);
        buffer->data[0].size = frames * sizeof (uint16_t) * 2;

        this->event_cb (&this->handle, &event,this->user_data);

        spa_buffer_unref ((SpaBuffer *)event.data);
      }
      if (this->input_buffer) {
        if (this->input_buffer != &this->buffer.buffer) {
          /* FIXME, copy input */
        }
        spa_buffer_unref (this->input_buffer);
        this->input_buffer = NULL;
      }

      commitres = snd_pcm_mmap_commit(handle, offset, frames);
      if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
        if ((err = xrun_recovery(handle, commitres >= 0 ? -EPIPE : commitres)) < 0) {
          printf("MMAP commit error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
        }
        first = 1;
      }
      size -= frames;
    }
  }
  return NULL;
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

  if (spa_alsa_open (this) < 0)
    return -1;

  CHECK (set_hwparams (this), "hwparams");
  CHECK (set_swparams (this), "swparams");

  snd_pcm_dump (this->state.handle, this->state.output);

  state->running = true;
  if ((err = pthread_create (&state->thread, NULL, direct_loop, this)) != 0) {
    printf ("can't create thread: %d", err);
    state->running = false;
  }
  return err;
}

static int
spa_alsa_stop (SpaALSASink *this)
{
  SpaALSAState *state = &this->state;

  if (state->running) {
    state->running = false;
    pthread_join (state->thread, NULL);
  }
  spa_alsa_close (this);

  return 0;
}
