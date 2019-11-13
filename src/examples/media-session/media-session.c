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

int sm_monitor_start(struct pw_remote *remote);
int sm_policy_start(struct pw_remote *remote);
int sm_policy_ep_start(struct pw_remote *remote);

struct impl {
	struct pw_main_loop *loop;
	struct pw_core *core;

	struct pw_remote *monitor_remote;
	struct spa_hook monitor_listener;

	struct pw_remote *policy_remote;
	struct spa_hook policy_listener;
};

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		pw_main_loop_quit(impl->loop);
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

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };
	int res;

	pw_init(&argc, &argv);

	impl.loop = pw_main_loop_new(NULL);
	impl.core = pw_core_new(pw_main_loop_get_loop(impl.loop), NULL, 0);

	pw_core_add_spa_lib(impl.core, "api.bluez5.*", "bluez5/libspa-bluez5");
	pw_core_add_spa_lib(impl.core, "api.alsa.*", "alsa/libspa-alsa");
	pw_core_add_spa_lib(impl.core, "api.v4l2.*", "v4l2/libspa-v4l2");

	impl.monitor_remote = pw_remote_new(impl.core, NULL, 0);
	pw_remote_add_listener(impl.monitor_remote, &impl.monitor_listener, &remote_events, &impl);

	impl.policy_remote = pw_remote_new(impl.core, NULL, 0);
	pw_remote_add_listener(impl.policy_remote, &impl.policy_listener, &remote_events, &impl);

	pw_module_load(impl.core, "libpipewire-module-client-device", NULL, NULL);
	pw_module_load(impl.core, "libpipewire-module-adapter", NULL, NULL);
	pw_module_load(impl.core, "libpipewire-module-metadata", NULL, NULL);
	pw_module_load(impl.core, "libpipewire-module-session-manager", NULL, NULL);

	sm_monitor_start(impl.monitor_remote);
//	sm_policy_start(impl.policy_remote);
	sm_policy_ep_start(impl.policy_remote);

	if ((res = pw_remote_connect(impl.monitor_remote)) < 0)
		return res;
	if ((res = pw_remote_connect(impl.policy_remote)) < 0)
		return res;

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
