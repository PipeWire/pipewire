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

#include "media-session.h"

#define NAME "media-session"

#define DEFAULT_IDLE_SECONDS	3

void * sm_stream_monitor_start(struct sm_media_session *sess);

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

	struct sm_media_session *session;

	struct monitor bluez5_monitor;
	struct monitor alsa_monitor;
	struct monitor v4l2_monitor;

	struct sm_metadata *metadata;

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

static void start_services(struct impl *impl)
{
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_core_get_support(impl->session->core, &n_support);

	impl->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	if (impl->dbus)
		impl->dbus_connection = spa_dbus_get_connection(impl->dbus, DBUS_BUS_SESSION);
	if (impl->dbus_connection)
		impl->conn = spa_dbus_connection_get(impl->dbus_connection);
	if (impl->conn == NULL)
		pw_log_warn("no dbus connection, device reservation disabled");
	else
		pw_log_debug("got dbus connection %p", impl->conn);

	sm_media_session_export(impl->session,
			PW_TYPE_INTERFACE_Metadata,
			NULL,
			impl->metadata,
			0);

	bluez5_start_monitor(impl, &impl->bluez5_monitor);
	alsa_start_monitor(impl, &impl->alsa_monitor);
	alsa_start_midi_bridge(impl);
	alsa_start_jack_device(impl);
	v4l2_start_monitor(impl, &impl->v4l2_monitor);
	sm_stream_monitor_start(impl->session);
}

int sm_monitor_start(struct sm_media_session *sess)
{
	struct impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return -errno;

	impl->session = sess;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	impl->metadata = sm_metadata_new(NULL);

	start_services(impl);

	return 0;
}
