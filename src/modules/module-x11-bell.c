/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

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

#ifdef HAVE_XFIXES_6
#include <X11/extensions/Xfixes.h>
#endif

#include <canberra.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

/** \page page_module_x11_bell X11 Bell
 *
 * The `x11-bell` module intercept the X11 bell events and uses libcanberra to
 * play a sound.
 *
 * ## Module Name
 *
 * `libpipewire-module-x11-bell`
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

/* libcanberra is not thread safe when doing ca_context_create()
 * and so we need a global lock */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct impl {
	struct pw_context *context;
	struct pw_thread_loop *thread_loop;
	struct pw_loop *thread_loop_loop;
	struct pw_loop *loop;
	struct spa_source *source;

	struct pw_properties *properties;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	Display *display;
};

static int play_sample(struct impl *impl)
{
	const char *sample = NULL;
	ca_context *ca;
	int res;

	pthread_mutex_lock(&lock);
	if (impl->properties)
		sample = pw_properties_get(impl->properties, "sample.name");
	if (sample == NULL)
		sample = "bell-window-system";

	pw_log_info("play sample %s", sample);

	if ((res = ca_context_create(&ca)) < 0) {
		pw_log_error("canberra context create error: %s", ca_strerror(res));
		res = -EIO;
		goto exit;
	}
	if ((res = ca_context_set_driver(ca, "pulse")) < 0) {
		pw_log_error("canberra context set backend error: %s", ca_strerror(res));
		res = -EIO;
		goto exit_destroy;
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
		res = -EIO;
		goto exit_destroy;
	}

exit_destroy:
	ca_context_destroy(ca);
exit:
	pthread_mutex_unlock(&lock);
	return res;
}

static int do_play_sample(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	play_sample(user_data);
	return 0;
}

static void display_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	XEvent e;

	while (XPending(impl->display)) {
		XNextEvent(impl->display, &e);

		if (((XkbEvent*) &e)->any.xkb_type != XkbBellNotify)
			continue;

		pw_loop_invoke(impl->thread_loop_loop, do_play_sample, 0, NULL, 0, false, impl);
	}
}

#ifdef HAVE_XSETIOERROREXITHANDLER
static void x11_io_error_exit_handler(Display *display, void *data)
{
	struct impl *impl = data;

	spa_assert(display == impl->display);

	pw_log_warn("X11 display (%s) has encountered a fatal I/O error", DisplayString(display));

	pw_loop_destroy_source(impl->loop, impl->source);
	impl->source = NULL;

	pw_impl_module_schedule_destroy(impl->module);
}
#endif

static int x11_connect(struct impl *impl, const char *name)
{
	int major, minor;
	unsigned int auto_ctrls, auto_values;

	if (!(impl->display = XOpenDisplay(name))) {
		pw_log_info("XOpenDisplay() failed. Uninstall or disable the module-x11-bell module");
		return -EHOSTDOWN;
	}

	impl->source = pw_loop_add_io(impl->loop,
			ConnectionNumber(impl->display),
			SPA_IO_IN, false, display_io, impl);
	if (!impl->source)
		return -errno;

#ifdef HAVE_XSETIOERROREXITHANDLER
	XSetIOErrorExitHandler(impl->display, x11_io_error_exit_handler, impl);
#endif

#ifdef HAVE_XFIXES_6
	XFixesSetClientDisconnectMode(impl->display, XFixesClientDisconnectFlagTerminate);
#endif

	major = XkbMajorVersion;
	minor = XkbMinorVersion;

	if (!XkbLibraryVersion(&major, &minor)) {
		pw_log_error("XkbLibraryVersion() failed");
		return -EIO;
	}

	major = XkbMajorVersion;
	minor = XkbMinorVersion;

	if (!XkbQueryExtension(impl->display, NULL, NULL, NULL, &major, &minor)) {
		pw_log_error("XkbQueryExtension() failed");
		return -EIO;
	}

	XkbSelectEvents(impl->display, XkbUseCoreKbd, XkbBellNotifyMask, XkbBellNotifyMask);
	auto_ctrls = auto_values = XkbAudibleBellMask;
	XkbSetAutoResetControls(impl->display, XkbAudibleBellMask, &auto_ctrls, &auto_values);
	XkbChangeEnabledControls(impl->display, XkbUseCoreKbd, XkbAudibleBellMask, 0);

	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	if (impl->module)
		spa_hook_remove(&impl->module_listener);

	if (impl->source)
		pw_loop_destroy_source(impl->loop, impl->source);

	if (impl->display)
		XCloseDisplay(impl->display);

	if (impl->thread_loop)
		pw_thread_loop_destroy(impl->thread_loop);

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
	{ PW_KEY_MODULE_USAGE,	"( sink.name=<name for the sink> ) "
				"( sample.name=<the sample name> ) "
				"( x11.display=<the X11 display> ) "
				".x11.xauthority=<the X11 XAuthority> )" },
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
	impl->loop = pw_context_get_main_loop(context);

	impl->thread_loop = pw_thread_loop_new("X11 Bell", NULL);
	if (impl->thread_loop == NULL) {
		res = -errno;
		pw_log_error("can't create thread loop: %m");
		goto error;
	}
	impl->thread_loop_loop = pw_thread_loop_get_loop(impl->thread_loop);
	impl->properties = args ? pw_properties_new_string(args) : NULL;

	impl->module = module;
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

	res = x11_connect(impl, name);
	if (res < 0)
		goto error;

	return 0;
error:
	module_destroy(impl);
	return res;

}

static int x11_error_handler(Display *display, XErrorEvent *error)
{
	pw_log_warn("X11 error handler called on display %s with error %d",
		    DisplayString(display), error->error_code);
	return 0;
}

static int x11_io_error_handler(Display *display)
{
	pw_log_warn("X11 I/O error handler called on display %s", DisplayString(display));
	return 0;
}

__attribute__((constructor))
static void set_x11_handlers(void)
{
	{
		XErrorHandler prev = XSetErrorHandler(NULL);
		XErrorHandler def = XSetErrorHandler(x11_error_handler);

		if (prev != def)
			XSetErrorHandler(prev);
	}

	{
		XIOErrorHandler prev = XSetIOErrorHandler(NULL);
		XIOErrorHandler def = XSetIOErrorHandler(x11_io_error_handler);

		if (prev != def)
			XSetIOErrorHandler(prev);
	}
}

__attribute__((destructor))
static void restore_x11_handlers(void)
{
	{
		XErrorHandler prev = XSetErrorHandler(NULL);
		if (prev != x11_error_handler)
			XSetErrorHandler(prev);
	}

	{
		XIOErrorHandler prev = XSetIOErrorHandler(NULL);
		if (prev != x11_io_error_handler)
			XSetIOErrorHandler(prev);
	}
}
