/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>
#include <spa/support/dbus.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "extensions/session-manager.h"

#include <dbus/dbus.h>

#define NAME "media-session"

#define DEFAULT_IDLE_SECONDS	3

struct impl;

struct monitor {
	struct impl *impl;

	struct spa_handle *handle;

	struct spa_device *monitor;
	struct spa_hook listener;

	struct spa_list object_list;
};

struct impl {
	struct timespec now;

	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;

	struct monitor bluez5_monitor;
	struct monitor alsa_monitor;
	struct monitor v4l2_monitor;

	struct sm_metadata *metadata;

	struct pw_client_session_proxy *client_session;
	struct spa_hook client_session_listener;
	struct pw_session_info client_session_info;

	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct pw_proxy *midi_bridge;

	struct spa_source *jack_timeout;
	struct pw_proxy *jack_device;
};

struct alsa_object;

static int setup_alsa_endpoint(struct alsa_object *obj);

#include "alsa-monitor.c"
#include "alsa-endpoint.c"
#include "v4l2-monitor.c"
#include "bluez-monitor.c"
#include "metadata.c"

static int client_session_set_id(void *object, uint32_t id)
{
	struct impl *impl = object;

	pw_log_debug("got sesssion id:%d", id);
	impl->client_session_info.id = id;

	pw_client_session_proxy_update(impl->client_session,
			PW_CLIENT_SESSION_UPDATE_INFO,
			0, NULL,
			&impl->client_session_info);
	return 0;
}

static int client_session_set_param(void *object, uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct impl *impl = object;
	pw_proxy_error((struct pw_proxy*)impl->client_session,
			-ENOTSUP, "Session:SetParam not supported");
	return -ENOTSUP;
}

static int client_session_link_set_param(void *object, uint32_t link_id, uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct impl *impl = object;
	pw_proxy_error((struct pw_proxy*)impl->client_session,
			-ENOTSUP, "Session:LinkSetParam not supported");
	return -ENOTSUP;
}

static int client_session_create_link(void *object, const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int client_session_destroy_link(void *object, uint32_t link_id)
{
	return -ENOTSUP;
}

static int client_session_link_request_state(void *object, uint32_t link_id, uint32_t state)
{
	return -ENOTSUP;
}


static const struct pw_client_session_proxy_events client_session_events = {
	PW_VERSION_CLIENT_SESSION_PROXY_METHODS,
	.set_id = client_session_set_id,
	.set_param = client_session_set_param,
	.link_set_param = client_session_link_set_param,
	.create_link = client_session_create_link,
	.destroy_link = client_session_destroy_link,
	.link_request_state = client_session_link_request_state,
};

static void start_services(struct impl *impl)
{
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_core_get_support(impl->core, &n_support);

	impl->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	if (impl->dbus)
		impl->dbus_connection = spa_dbus_get_connection(impl->dbus, DBUS_BUS_SESSION);
	if (impl->dbus_connection)
		impl->conn = spa_dbus_connection_get(impl->dbus_connection);
	if (impl->conn == NULL)
		pw_log_warn("no dbus connection, device reservation disabled");
	else
		pw_log_debug("got dbus connection %p", impl->conn);

	pw_remote_export(impl->remote,
			PW_TYPE_INTERFACE_Metadata,
			NULL,
			impl->metadata,
			0);

	impl->client_session = pw_core_proxy_create_object(impl->core_proxy,
                                            "client-session",
                                            PW_TYPE_INTERFACE_ClientSession,
                                            PW_VERSION_CLIENT_SESSION_PROXY,
                                            NULL, 0);
	impl->client_session_info.version = PW_VERSION_SESSION_INFO;

	pw_client_session_proxy_add_listener(impl->client_session,
			&impl->client_session_listener,
			&client_session_events,
			impl);

	bluez5_start_monitor(impl, &impl->bluez5_monitor);
	alsa_start_monitor(impl, &impl->alsa_monitor);
	alsa_start_midi_bridge(impl);
	alsa_start_jack_device(impl);
	v4l2_start_monitor(impl, &impl->v4l2_monitor);
}

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		impl->core_proxy = pw_remote_get_core_proxy(impl->remote);
		start_services(impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		impl->core_proxy = NULL;
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int sm_monitor_start(struct pw_remote *remote)
{
	struct impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -errno;

	impl->core = pw_remote_get_core(remote);
	impl->remote = remote;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	impl->metadata = sm_metadata_new(NULL);

	pw_remote_add_listener(impl->remote, &impl->remote_listener, &remote_events, impl);

	return 0;
}
