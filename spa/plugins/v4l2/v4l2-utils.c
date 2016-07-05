#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>

#define CHECK(s,msg) if ((err = (s)) < 0) { printf (msg ": %s\n", snd_strerror(err)); return err; }


static int
spa_v4l2_open (SpaV4l2Source *this)
{
  struct stat st;

  fprintf (stderr, "Playback device is '%s'\n", this->props.device);

  if (stat (this->props.device, &st) < 0) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n",
            this->props.device, errno, strerror (errno));
    return -1;
  }

  if (!S_ISCHR (st.st_mode)) {
    fprintf(stderr, "%s is no device\n", this->props.device);
    return -1;
  }

  this->state.fd = open (this->props.device, O_RDWR | O_NONBLOCK, 0);

  if (this->state.fd == -1) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n",
            this->props.device, errno, strerror (errno));
    return -1;
  }
  return 0;
}

static int
spa_v4l2_close (SpaV4l2Source *this)
{
  if (close(this->state.fd))
    fprintf(stderr, "Cannot close %d, %s\n",
            errno, strerror (errno));

  this->state.fd = -1;
  return 0;
}

static void *
v4l2_loop (void *user_data)
{
  return NULL;
}

static int
spa_v4l2_start (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state;
  int err;

  state->running = true;
  if ((err = pthread_create (&state->thread, NULL, v4l2_loop, this)) != 0) {
    printf ("can't create thread: %d", err);
    state->running = false;
  }
  return err;
}

static int
spa_v4l2_stop (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state;

  if (state->running) {
    state->running = false;
    pthread_join (state->thread, NULL);
  }
  return 0;
}
