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
spa_xv_open (struct impl *this)
{
  struct port *port = &this->in_ports[0];

  if (port->opened)
    return 0;

  port->opened = true;

  return 0;
}

static int
spa_xv_set_format (struct impl *this, struct spa_video_info *info, bool try_only)
{
  if (spa_xv_open (this) < 0)
    return -1;

  return 0;
}

static int
spa_xv_close (struct impl *this)
{
  struct port *port = &this->in_ports[0];

  if (!port->opened)
    return 0;

  port->opened = false;

  return 0;
}

static int
spa_xv_start (struct impl *this)
{
  if (spa_xv_open (this) < 0)
    return -1;

  return 0;
}

static int
spa_xv_stop (struct impl *this)
{
  spa_xv_close (this);

  return 0;
}
