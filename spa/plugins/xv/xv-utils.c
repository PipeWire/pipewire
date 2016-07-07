#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

static int
spa_xv_open (SpaXvSink *this)
{
  SpaXvState *state = &this->state;

  if (state->opened)
    return 0;

  state->opened = true;

  return 0;
}

static int
spa_xv_set_format (SpaXvSink *this, SpaFormat *format, bool try_only)
{
  if (spa_xv_open (this) < 0)
    return -1;

  return 0;
}

static int
spa_xv_close (SpaXvSink *this)
{
  SpaXvState *state = &this->state;

  if (!state->opened)
    return 0;

  state->opened = false;

  return 0;
}

static int
spa_xv_start (SpaXvSink *this)
{
  if (spa_xv_open (this) < 0)
    return -1;

  return 0;
}

static int
spa_xv_stop (SpaXvSink *this)
{
  spa_xv_close (this);

  return 0;
}
