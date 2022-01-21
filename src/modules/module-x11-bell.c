/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/string.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>

#include <canberra.h>

#include "pipewire/pipewire.h"
#include "pipewire/impl.h"

/** \page page_module_x11_bell PipeWire Module: X11 Bell
 *
 * The `x11-bell` module intercept the X11 bell events and uses libcanberra to
 * play a sound.
 *
 * ## Module Options
 *
 * - `sink.name = <str>`: node.name of the sink to connect to
 * - `sample.name = <str>`: the name of the sample to play, default 'bell-window-system'
 * - `x11.display = <str>`: the X11 display to use
 * - `x11.xauthority = <str>`: the X11 XAuthority string placed in XAUTHORITY env
 *
 * ## General options
 *
 * There are no general options for this module.
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-x11-bell }
 *      args = {
 *          #sink.name = @DEFAULT_SINK@
 *          sample.name = "bell-window-system"
 *          #x11.display = ":1"
 *          #x11.xauthority = "test"
 * ]
 *\endcode
 *
 */

#define NAME "x11-bell"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct impl {
	struct pw_context *context;
	struct pw_thread_loop *thread_loop;
	struct pw_loop *loop;
	struct spa_source *source;

	struct pw_properties *properties;

	struct spa_hook module_listener;

	Display *display;
	int xkb_event_base;
};

static int play_sample(struct impl *impl, const char *sample)
{
	int res;
	ca_context *ca;

	if ((res = ca_context_create(&ca)) < 0) {
		pw_log_error("canberra context create error: %s", ca_strerror(res));
		res = -EIO;
		goto exit;
	}
	if ((res = ca_context_open(ca)) < 0) {
		pw_log_error("canberra context open error: %s", ca_strerror(res));
		res = -EIO;
		goto exit_destroy;
	}
	if ((res = ca_context_play(ca, 0,
			CA_PROP_EVENT_ID, sample,
			CA_PROP_MEDIA_NAME, "X11 bell event",
			CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
			NULL)) < 0) {
		pw_log_warn("can't play sample (%s): %s", sample, ca_strerror(res));
	}

exit_destroy:
	ca_context_destroy(ca);
exit:
	return res;
}
static void display_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	XEvent e;
	const char *sample = NULL;

	while (XPending(impl->display)) {
		XNextEvent(impl->display, &e);

		if (((XkbEvent*) &e)->any.xkb_type != XkbBellNotify)
			continue;

		if (impl->properties)
			sample = pw_properties_get(impl->properties, "sample.name");
		if (sample == NULL)
			sample = "bell-window-system";

		pw_log_debug("play sample %s", sample);
		play_sample(impl, sample);
	}
}

static void x11_close(struct impl *impl)
{
	if (impl->source) {
		pw_loop_destroy_source(impl->loop, impl->source);
		impl->source = NULL;
	}
	if (impl->display) {
		XCloseDisplay(impl->display);
		impl->display = NULL;
	}
}

static int x11_connect(struct impl *impl, const char *name)
{
	int res, major, minor;
	unsigned int auto_ctrls, auto_values;

	if (!(impl->display = XOpenDisplay(name))) {
		pw_log_warn("XOpenDisplay() failed");
		res = -EIO;
		goto error;
	}

	impl->source = pw_loop_add_io(impl->loop,
			ConnectionNumber(impl->display),
			SPA_IO_IN, false, display_io, impl);

	major = XkbMajorVersion;
	minor = XkbMinorVersion;

	if (!XkbLibraryVersion(&major, &minor)) {
		pw_log_warn("XkbLibraryVersion() failed");
		res = -EIO;
		goto error;
	}

	major = XkbMajorVersion;
	minor = XkbMinorVersion;

	if (!XkbQueryExtension(impl->display, NULL, &impl->xkb_event_base,
				NULL, &major, &minor)) {
		res = -EIO;
		pw_log_warn("XkbQueryExtension() failed");
		goto error;
	}

	XkbSelectEvents(impl->display, XkbUseCoreKbd, XkbBellNotifyMask, XkbBellNotifyMask);
	auto_ctrls = auto_values = XkbAudibleBellMask;
	XkbSetAutoResetControls(impl->display, XkbAudibleBellMask, &auto_ctrls, &auto_values);
	XkbChangeEnabledControls(impl->display, XkbUseCoreKbd, XkbAudibleBellMask, 0);

	res = 0;
error:
	if (res < 0)
		x11_close(impl);
	return res;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);

	if (impl->thread_loop)
		pw_thread_loop_lock(impl->thread_loop);

	x11_close(impl);

	if (impl->thread_loop) {
		pw_thread_loop_unlock(impl->thread_loop);
		pw_thread_loop_destroy(impl->thread_loop);
	}

	pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static const struct spa_dict_item module_x11_bell_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "X11 Bell interceptor" },
	{ PW_KEY_MODULE_USAGE,	"sink.name=<name for the sink> "
				"sample.name=<the sample name> "
				"x11.display=<the X11 display> "
				"x11.xauthority=<the X11 XAuthority> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};
SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	const char *name = NULL, *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->context = context;
	impl->thread_loop = pw_thread_loop_new("X11 Bell", NULL);
	if (impl->thread_loop == NULL) {
		res = -errno;
		pw_log_error("can't create thread loop: %m");
		goto error;
	}
	impl->loop = pw_thread_loop_get_loop(impl->thread_loop);
	impl->properties = args ? pw_properties_new_string(args) : NULL;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_x11_bell_info));

	if (impl->properties) {
		if ((str = pw_properties_get(impl->properties, "x11.xauthority")) != NULL) {
			if (setenv("XAUTHORITY", str, 1)) {
				res = -errno;
				pw_log_error("XAUTHORITY setenv failed: %m");
				goto error;
			}
		}
		name = pw_properties_get(impl->properties, "x11.display");
	}

	/* we need to use a thread loop because this module will connect
	 * to pipewire eventually and will then block the mainloop. */
	pw_thread_loop_start(impl->thread_loop);

	pw_thread_loop_lock(impl->thread_loop);
	x11_connect(impl, name);
	pw_thread_loop_unlock(impl->thread_loop);

	return 0;
error:
	module_destroy(impl);
	return res;

}
