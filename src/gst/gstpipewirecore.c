/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"
#include <unistd.h>
#include <fcntl.h>

#include <spa/utils/result.h>

#include "gstpipewirecore.h"

/* a list of global cores indexed by fd. */
G_LOCK_DEFINE_STATIC (cores_lock);
static GList *cores;

static void
on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  GstPipeWireCore *core = data;

  pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
          id, seq, res, spa_strerror(res), message);

  if (id == PW_ID_CORE) {
    core->last_error = res;
  }
  pw_thread_loop_signal(core->loop, FALSE);
}

static void on_core_done (void *data, uint32_t id, int seq)
{
  GstPipeWireCore * core = data;
  if (id == PW_ID_CORE) {
    core->last_seq = seq;
    pw_thread_loop_signal (core->loop, FALSE);
  }
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_core_done,
  .error = on_core_error,
};

static GstPipeWireCore *make_core (int fd)
{
  GstPipeWireCore *core;

  core = g_new (GstPipeWireCore, 1);
  core->refcount = 1;
  core->fd = fd;
  core->loop = pw_thread_loop_new ("pipewire-main-loop", NULL);
  core->context = pw_context_new (pw_thread_loop_get_loop(core->loop), NULL, 0);
  core->last_seq = -1;
  core->last_error = 0;
  GST_DEBUG ("loop %p context %p", core->loop, core->context);

  if (pw_thread_loop_start (core->loop) < 0)
    goto mainloop_failed;

  pw_thread_loop_lock (core->loop);

  if (fd == -1)
    core->core = pw_context_connect (core->context, NULL, 0);
  else
    core->core = pw_context_connect_fd (core->context, fcntl(fd, F_DUPFD_CLOEXEC, 3), NULL, 0);

  if (core->core == NULL)
    goto connection_failed;

  pw_core_add_listener(core->core,
                       &core->core_listener,
                       &core_events,
                       core);

  pw_thread_loop_unlock (core->loop);

  return core;

mainloop_failed:
  {
    GST_ERROR ("error starting mainloop");
    pw_context_destroy (core->context);
    pw_thread_loop_destroy (core->loop);
    g_free (core);
    return NULL;
  }
connection_failed:
  {
    GST_ERROR ("error connect: %m");
    pw_thread_loop_unlock (core->loop);
    pw_context_destroy (core->context);
    pw_thread_loop_destroy (core->loop);
    g_free (core);
    return NULL;
  }
}

typedef struct {
  int fd;
} FindData;

static gint
core_find (GstPipeWireCore * core, FindData * data)
{
  /* fd's must match */
  if (core->fd == data->fd)
    return 0;
  return 1;
}

GstPipeWireCore *gst_pipewire_core_get (int fd)
{
  GstPipeWireCore *core;
  FindData data;
  GList *found;

  data.fd = fd;

  G_LOCK (cores_lock);
  found = g_list_find_custom (cores, &data, (GCompareFunc) core_find);
  if (found != NULL) {
    core = (GstPipeWireCore *) found->data;
    core->refcount++;
    GST_DEBUG ("found core %p", core);
  } else {
    core = make_core(fd);
    if (core != NULL) {
      GST_DEBUG ("created core %p", core);
      /* add to list on success */
      cores = g_list_prepend (cores, core);
    } else {
      GST_WARNING ("could not create core");
    }
  }
  G_UNLOCK (cores_lock);

  return core;
}

static void do_sync(GstPipeWireCore * core)
{
  struct timespec abstime;
  core->pending_seq = pw_core_sync(core->core, 0, core->pending_seq);
  pw_thread_loop_get_time (core->loop, &abstime,
              GST_PIPEWIRE_DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);
  while (true) {
    if (core->last_seq == core->pending_seq || core->last_error < 0)
      break;
    if (pw_thread_loop_timed_wait_full (core->loop, &abstime) < 0)
      break;
  }
}

void gst_pipewire_core_release (GstPipeWireCore *core)
{
  gboolean zero;

  G_LOCK (cores_lock);
  core->refcount--;
  if ((zero = (core->refcount == 0))) {
    GST_DEBUG ("closing core %p", core);
    /* remove from list, we can release the mutex after removing the connection
     * from the list because after that, nobody can access the connection anymore. */
    cores = g_list_remove (cores, core);
  }
  G_UNLOCK (cores_lock);

  if (zero) {
    pw_thread_loop_lock (core->loop);
    do_sync(core);

    pw_core_disconnect (core->core);
    pw_thread_loop_unlock (core->loop);
    pw_thread_loop_stop (core->loop);
    pw_context_destroy (core->context);
    pw_thread_loop_destroy (core->loop);

    free(core);
  }
}
