/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <dbus/dbus.h>

#include "pipewire/client/utils.h"
#include "pipewire/server/core.h"
#include "pipewire/server/module.h"

struct impl {
  struct pw_core       *core;
  struct pw_properties *properties;

  DBusConnection  *bus;

  struct pw_listener    global_added;
  struct pw_listener    global_removed;

  struct spa_list       client_list;
  struct pw_access      access;

  struct spa_source       *dispatch_event;
};

struct client_info {
  struct impl      *impl;
  struct spa_list   link;
  struct pw_client *client;
  bool              is_sandboxed;
  struct spa_list   async_pending;
};

struct async_pending {
  struct spa_list     link;
  bool                handled;
  struct client_info *info;
  char               *handle;
  struct pw_access_data *access_data;
};

static struct client_info *
find_client_info (struct impl *impl, struct pw_client *client)
{
  struct client_info *info;

  spa_list_for_each (info, &impl->client_list, link) {
    if (info->client == client)
      return info;
  }
  return NULL;
}

static void
close_request (struct async_pending *p)
{
  DBusMessage *m = NULL;
  struct impl *impl = p->info->impl;

  pw_log_debug ("pending %p: handle %s", p, p->handle);

  if (!(m = dbus_message_new_method_call ("org.freedesktop.portal.Request",
                                          p->handle,
                                          "org.freedesktop.portal.Request",
                                          "Close"))) {
    pw_log_error ("Failed to create message");
    return;
  }

  if (!dbus_connection_send (impl->bus, m, NULL))
    pw_log_error ("Failed to send message");

  dbus_message_unref(m);
}

static struct async_pending *
find_pending (struct client_info *cinfo, const char *handle)
{
  struct async_pending *p;

  spa_list_for_each (p, &cinfo->async_pending, link) {
    if (strcmp (p->handle, handle) == 0)
      return p;
  }
  return NULL;
}

static void
free_pending (struct pw_access_data *d)
{
  struct async_pending *p = d->user_data;

  if (!p->handled)
    close_request(p);

  pw_log_debug ("pending %p: handle %s", p, p->handle);
  spa_list_remove (&p->link);
  free (p->handle);
}

static void
add_pending (struct client_info *cinfo, const char *handle, struct pw_access_data *access_data)
{
  struct async_pending *p;
  struct pw_access_data *ad;

  ad = access_data->async_copy (access_data, sizeof (struct async_pending));
  ad->free_cb = free_pending;

  p = ad->user_data;
  p->info = cinfo;
  p->handle = strdup (handle);
  p->access_data = ad;
  p->handled = false;
  pw_log_debug ("pending %p: handle %s", p, handle);

  spa_list_insert (cinfo->async_pending.prev, &p->link);
}

static void
client_info_free (struct client_info *cinfo)
{
  struct async_pending *p, *tmp;

  spa_list_for_each_safe (p, tmp, &cinfo->async_pending, link) {
    p->access_data->res = SPA_RESULT_NO_PERMISSION;
    p->access_data->complete_cb (p->access_data);
  }
  spa_list_remove (&cinfo->link);
  free (cinfo);
}

static bool
client_is_sandboxed (struct pw_client *cl)
{
  char data[2048], *ptr;
  size_t n, size;
  const char *state = NULL;
  const char *current;
  bool result;
  int fd;
  pid_t pid;

  if (cl->ucred_valid) {
    pw_log_info ("client has trusted pid %d", cl->ucred.pid);
  }
  else {
    pw_log_info ("no trusted pid found, assuming not sandboxed\n");
    return false;
  }

  pid = cl->ucred.pid;

  sprintf (data, "/proc/%u/cgroup", pid);
  fd = open (data, O_RDONLY | O_CLOEXEC, 0);
  if (fd == -1)
    return false;

  size = sizeof (data);
  ptr = data;

  while (size > 0) {
    int r;

    if ((r = read (fd, data, size)) < 0) {
      if (errno == EINTR)
        continue;
      else
        break;
    }
    if (r == 0)
      break;

    ptr += r;
    size -= r;
  }
  close (fd);

  result = false;
  while ((current = pw_split_walk (data, "\n", &n, &state)) != NULL) {
    if (strncmp (current, "1:name=systemd:", strlen ("1:name=systemd:")) == 0) {
      const char *p = strstr (current, "flatpak-");
      if (p && p - current < n) {
        pw_log_info ("found a flatpak cgroup, assuming sandboxed\n");
        result = true;
        break;
      }
    }
  }
  return result;
}

static bool
check_global_owner (struct pw_core   *core,
                    struct pw_client *client,
                    struct pw_global *global)
{
  if (global == NULL)
    return false;

  if (global->owner == NULL)
    return true;

  if (global->owner->ucred.uid == client->ucred.uid)
    return true;

  return false;
}

static int
do_view_global (struct pw_access      *access,
                struct pw_client      *client,
                struct pw_global      *global)
{
  if (global->type == client->core->type.link) {
    struct pw_link *link = global->object;

    /* we must be able to see both nodes */
    if (link->output && !check_global_owner (client->core, client, link->output->node->global))
      return SPA_RESULT_ERROR;

    if (link->input && !check_global_owner (client->core, client, link->input->node->global))
      return SPA_RESULT_ERROR;
  }
  else if (!check_global_owner (client->core, client, global))
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static int
do_create_node (struct pw_access      *access,
                struct pw_access_data  *data,
                const char       *factory_name,
                const char       *name,
                struct pw_properties  *properties)
{
  struct impl *impl = SPA_CONTAINER_OF (access, struct impl, access);
  struct client_info *cinfo = find_client_info (impl, data->resource->client);

  if (cinfo->is_sandboxed)
    data->res = SPA_RESULT_NO_PERMISSION;
  else
    data->res = SPA_RESULT_OK;

  data->complete_cb (data);
  return SPA_RESULT_OK;
}


static DBusHandlerResult
portal_response (DBusConnection *connection, DBusMessage *msg, void *user_data)
{
  struct client_info *cinfo = user_data;

  if (dbus_message_is_signal (msg, "org.freedesktop.portal.Request", "Response")) {
    uint32_t response = 2;
    DBusError error;
    struct async_pending *p;
    struct pw_access_data *d;

    dbus_error_init (&error);

    dbus_connection_remove_filter (connection, portal_response, cinfo);

    if (!dbus_message_get_args (msg, &error, DBUS_TYPE_UINT32, &response, DBUS_TYPE_INVALID)) {
        pw_log_error("failed to parse Response: %s", error.message);
        dbus_error_free(&error);
    }

    p = find_pending (cinfo, dbus_message_get_path (msg));
    if (p == NULL)
        return DBUS_HANDLER_RESULT_HANDLED;

    p->handled = true;
    d = p->access_data;

    pw_log_debug ("portal check result: %d", response);

    d->res = response == 0 ? SPA_RESULT_OK : SPA_RESULT_NO_PERMISSION;
    d->complete_cb (d);

    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int
do_create_client_node (struct pw_access      *access,
                       struct pw_access_data  *data,
                       const char       *name,
                       struct pw_properties  *properties)
{
  struct impl *impl = SPA_CONTAINER_OF (access, struct impl, access);
  struct client_info *cinfo = find_client_info (impl, data->resource->client);
  DBusMessage *m = NULL, *r = NULL;
  DBusError error;
  pid_t pid;
  DBusMessageIter msg_iter;
  DBusMessageIter dict_iter;
  const char *handle;
  const char *device;

  if (!cinfo->is_sandboxed) {
    data->res = SPA_RESULT_OK;
    data->complete_cb (data);
    return SPA_RESULT_OK;
  }

  pw_log_info ("ask portal for client %p", cinfo->client);

  dbus_error_init(&error);

  if (!(m = dbus_message_new_method_call ("org.freedesktop.portal.Desktop",
                                          "/org/freedesktop/portal/desktop",
                                          "org.freedesktop.portal.Device",
                                          "AccessDevice")))
    goto no_method_call;

  device = "camera";

  pid = cinfo->client->ucred.pid;
  if (!dbus_message_append_args(m,
                                DBUS_TYPE_UINT32, &pid,
                                DBUS_TYPE_INVALID))
    goto message_failed;

  dbus_message_iter_init_append(m, &msg_iter);
  dbus_message_iter_open_container (&msg_iter, DBUS_TYPE_ARRAY, "s", &dict_iter);
  dbus_message_iter_append_basic (&dict_iter, DBUS_TYPE_STRING, &device);
  dbus_message_iter_close_container (&msg_iter, &dict_iter);

  dbus_message_iter_open_container (&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
  dbus_message_iter_close_container (&msg_iter, &dict_iter);

  if (!(r = dbus_connection_send_with_reply_and_block (impl->bus, m, -1, &error)))
    goto send_failed;

  dbus_message_unref(m);

  if (!dbus_message_get_args (r, &error, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID))
    goto parse_failed;

  dbus_message_unref (r);

  dbus_bus_add_match (impl->bus,
                     "type='signal',interface='org.freedesktop.portal.Request'",
                     &error);
  dbus_connection_flush (impl->bus);
  if (dbus_error_is_set (&error))
    goto subscribe_failed;

  dbus_connection_add_filter (impl->bus, portal_response, cinfo, NULL);

  add_pending (cinfo, handle, data);

  return SPA_RESULT_RETURN_ASYNC (0);

no_method_call:
  pw_log_error ("Failed to create message");
  return SPA_RESULT_NO_PERMISSION;
message_failed:
  dbus_message_unref(m);
  return SPA_RESULT_NO_PERMISSION;
send_failed:
  pw_log_error ("Failed to call portal: %s", error.message);
  dbus_error_free(&error);
  dbus_message_unref(m);
  return SPA_RESULT_NO_PERMISSION;
parse_failed:
  pw_log_error ("Failed to parse AccessDevice result: %s", error.message);
  dbus_error_free (&error);
  dbus_message_unref (r);
  return SPA_RESULT_NO_PERMISSION;
subscribe_failed:
  pw_log_error ("Failed to subscribe to Request signal: %s", error.message);
  dbus_error_free(&error);
  return SPA_RESULT_NO_PERMISSION;
}

static struct pw_access access_checks =
{
  do_view_global,
  do_create_node,
  do_create_client_node,
};

static void
on_global_added (struct pw_listener *listener,
                 struct pw_core     *core,
                 struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_added);

  if (global->type == impl->core->type.client) {
    struct pw_client *client = global->object;
    struct client_info *cinfo;

    cinfo = calloc (1, sizeof (struct client_info));
    cinfo->impl = impl;
    cinfo->client = client;
    cinfo->is_sandboxed = client_is_sandboxed (client);
    cinfo->is_sandboxed = true;
    spa_list_init (&cinfo->async_pending);

    spa_list_insert (impl->client_list.prev, &cinfo->link);

    pw_log_debug ("module %p: client %p added", impl, client);
  }
}

static void
on_global_removed (struct pw_listener *listener,
                   struct pw_core     *core,
                   struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_removed);

  if (global->type == impl->core->type.client) {
    struct pw_client *client = global->object;
    struct client_info *cinfo;

    if ((cinfo = find_client_info (impl, client)))
      client_info_free (cinfo);

    pw_log_debug ("module %p: client %p removed", impl, client);
  }
}

static void
dispatch_cb (struct spa_loop_utils *utils, struct spa_source *source, void *userdata)
{
  struct impl *impl = userdata;

  if (dbus_connection_dispatch (impl->bus) == DBUS_DISPATCH_COMPLETE)
    pw_loop_enable_idle (impl->core->main_loop->loop, source, false);
}

static void
dispatch_status (DBusConnection *conn, DBusDispatchStatus status, void *userdata)
{
  struct impl *impl = userdata;

  pw_loop_enable_idle (impl->core->main_loop->loop,
                          impl->dispatch_event,
                          status == DBUS_DISPATCH_COMPLETE ? false : true);
}

static inline enum spa_io
dbus_to_io (DBusWatch *watch)
{
  enum spa_io mask;
  unsigned int flags;

  /* no watch flags for disabled watches */
  if (!dbus_watch_get_enabled (watch))
    return 0;

  flags = dbus_watch_get_flags (watch);
  mask = SPA_IO_HUP | SPA_IO_ERR;

  if (flags & DBUS_WATCH_READABLE)
    mask |= SPA_IO_IN;
  if (flags & DBUS_WATCH_WRITABLE)
    mask |= SPA_IO_OUT;

  return mask;
}

static inline unsigned int
io_to_dbus (enum spa_io mask)
{
  unsigned int flags = 0;

  if (mask & SPA_IO_IN)
    flags |= DBUS_WATCH_READABLE;
  if (mask & SPA_IO_OUT)
    flags |= DBUS_WATCH_WRITABLE;
  if (mask & SPA_IO_HUP)
    flags |= DBUS_WATCH_HANGUP;
  if (mask & SPA_IO_ERR)
    flags |= DBUS_WATCH_ERROR;
  return flags;
}

static void
handle_io_event (struct spa_loop_utils *utils,
                 struct spa_source    *source,
                 int           fd,
                 enum spa_io         mask,
                 void         *userdata)
{
  DBusWatch *watch = userdata;

  if (!dbus_watch_get_enabled (watch)) {
    pw_log_warn("Asked to handle disabled watch: %p %i", (void*) watch, fd);
    return;
  }
  dbus_watch_handle (watch, io_to_dbus (mask));
}

static dbus_bool_t
add_watch (DBusWatch *watch, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;

  pw_log_debug ("add watch %p %d", watch, dbus_watch_get_unix_fd (watch));

  /* we dup because dbus tends to add the same fd multiple times and our epoll
   * implementation does not like that */
  source = pw_loop_add_io (impl->core->main_loop->loop,
                              dup (dbus_watch_get_unix_fd (watch)),
                              dbus_to_io (watch),
                              true,
                              handle_io_event,
                              watch);

  dbus_watch_set_data (watch, source, NULL);
  return TRUE;
}

static void
remove_watch (DBusWatch *watch, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;

  if ((source = dbus_watch_get_data (watch)))
    pw_loop_destroy_source (impl->core->main_loop->loop, source);
}

static void
toggle_watch (DBusWatch *watch, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;

  source = dbus_watch_get_data (watch);

  pw_loop_update_io (impl->core->main_loop->loop,
                        source,
                        dbus_to_io (watch));
}

static void
handle_timer_event (struct spa_loop_utils *utils, struct spa_source *source, void *userdata)
{
  DBusTimeout *timeout = userdata;
  uint64_t t;
  struct timespec ts;

  if (dbus_timeout_get_enabled (timeout)) {
    t = dbus_timeout_get_interval (timeout) * SPA_NSEC_PER_MSEC;
    ts.tv_sec = t / SPA_NSEC_PER_SEC;
    ts.tv_nsec = t % SPA_NSEC_PER_SEC;
    spa_loop_utils_update_timer (utils, source, &ts, NULL, false);

    dbus_timeout_handle (timeout);
  }
}

static dbus_bool_t
add_timeout (DBusTimeout *timeout, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;
  struct timespec ts;
  uint64_t t;

  if (!dbus_timeout_get_enabled (timeout))
    return FALSE;

  source = pw_loop_add_timer (impl->core->main_loop->loop,
                                 handle_timer_event,
                                 timeout);

  dbus_timeout_set_data (timeout, source, NULL);

  t = dbus_timeout_get_interval (timeout) * SPA_NSEC_PER_MSEC;
  ts.tv_sec = t / SPA_NSEC_PER_SEC;
  ts.tv_nsec = t % SPA_NSEC_PER_SEC;
  pw_loop_update_timer (impl->core->main_loop->loop,
                           source,
                           &ts,
                           NULL,
                           false);
  return TRUE;
}

static void
remove_timeout (DBusTimeout *timeout, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;

  if ((source = dbus_timeout_get_data (timeout)))
    pw_loop_destroy_source (impl->core->main_loop->loop, source);
}

static void
toggle_timeout (DBusTimeout *timeout, void *userdata)
{
  struct impl *impl = userdata;
  struct spa_source *source;
  struct timespec ts, *tsp;

  source = dbus_timeout_get_data (timeout);

  if (dbus_timeout_get_enabled (timeout)) {
    uint64_t t = dbus_timeout_get_interval (timeout) * SPA_NSEC_PER_MSEC;
    ts.tv_sec = t / SPA_NSEC_PER_SEC;
    ts.tv_nsec = t % SPA_NSEC_PER_SEC;
    tsp = &ts;
  }
  else {
    tsp = NULL;
  }
  pw_loop_update_timer (impl->core->main_loop->loop, source, tsp, NULL, false);
}

static void
wakeup_main (void *userdata)
{
  struct impl *impl = userdata;

  pw_loop_enable_idle (impl->core->main_loop->loop,
                          impl->dispatch_event,
                          true);
}

static struct impl *
module_new (struct pw_core       *core,
            struct pw_properties *properties)
{
  struct impl *impl;
  DBusError error;

  dbus_error_init(&error);

  impl = calloc (1, sizeof (struct impl));
  pw_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;
  impl->access = access_checks;

  impl->bus = dbus_bus_get_private (DBUS_BUS_SESSION, &error);
  if (impl->bus == NULL)
    goto error;

  impl->dispatch_event = pw_loop_add_idle (core->main_loop->loop,
                                              false,
                                              dispatch_cb,
                                              impl);

  dbus_connection_set_exit_on_disconnect (impl->bus, false);
  dbus_connection_set_dispatch_status_function (impl->bus, dispatch_status, impl, NULL);
  dbus_connection_set_watch_functions (impl->bus, add_watch, remove_watch, toggle_watch, impl, NULL);
  dbus_connection_set_timeout_functions (impl->bus, add_timeout, remove_timeout, toggle_timeout, impl, NULL);
  dbus_connection_set_wakeup_main_function (impl->bus, wakeup_main, impl, NULL);

  core->access = &impl->access;

  spa_list_init (&impl->client_list);

  pw_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pw_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

  return impl;

error:
  pw_log_error ("Failed to connect to system bus: %s", error.message);
  dbus_error_free (&error);
  return NULL;
}

#if 0
static void
module_destroy (struct impl *impl)
{
  pw_log_debug ("module %p: destroy", impl);

  dbus_connection_close (impl->bus);
  dbus_connection_unref (impl->bus);
  free (impl);
}
#endif

bool
pipewire__module_init (struct pw_module * module, const char * args)
{
  module_new (module->core, NULL);
  return true;
}
