/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2016 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2019 Red Hat Inc. */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <dbus/dbus.h>

#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/support/dbus.h>

#include "pipewire/context.h"
#include "pipewire/impl-client.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/utils.h"

/** \page page_module_jackdbus_detect JACK DBus detect
 *
 * Automaticall creates a sink/source when a jackdbus server is started
 * and connect to JACK.
 *
 * ## Module Name
 *
 * `libpipewire-module-jackdbus-detect`
 *
 * ## Module Options
 *
 * There are no module-specific options, all arguments are passed to
 * \ref page_module_jack_tunnel.
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-jackdbus-detect
 *      args {
 *         #jack.server    = null
 *         #tunnel.mode    = duplex
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         source.props = {
 *             # extra sink properties
 *         }
 *         sink.props = {
 *             # extra sink properties
 *         }
 *      }
 *  }
 * ]
 *\endcode
 *
 */

#define NAME "jackdbus-detect"

#define JACK_SERVICE_NAME "org.jackaudio.service"
#define JACK_INTERFACE_NAME "org.jackaudio.JackControl"
#define JACK_INTERFACE_PATH "/org/jackaudio/Controller"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct impl {
	struct pw_context *context;
	struct pw_properties *properties;

	struct spa_dbus_connection *conn;
	DBusConnection *bus;

	struct spa_hook module_listener;

	DBusPendingCall *pending_call;
	bool is_started;

	struct pw_impl_module *jack_tunnel;
	struct spa_hook tunnel_listener;
};

static void tunnelmodule_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->tunnel_listener);
	impl->jack_tunnel = NULL;
}

static const struct pw_impl_module_events tunnelmodule_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = tunnelmodule_destroy,
};

static int load_jack_tunnel(struct impl *impl)
{
	FILE *f;
	char *args;
	size_t size;
	int res = 0;

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	fprintf(f, "{");
	if (impl->properties != NULL)
		pw_properties_serialize_dict(f, &impl->properties->dict, 0);
	fprintf(f, " }");
	fclose(f);

	pw_log_info("loading module args:'%s'", args);
	impl->jack_tunnel = pw_context_load_module(impl->context,
			"libpipewire-module-jack-tunnel",
			args, NULL);
	free(args);

	if (impl->jack_tunnel == NULL) {
		res = -errno;
		pw_log_error("Can't create tunnel: %m");
		goto done;
	}

	pw_impl_module_add_listener(impl->jack_tunnel,
			&impl->tunnel_listener, &tunnelmodule_events, impl);
done:
	return res;
}

static void unload_jack_tunnel(struct impl *impl)
{
	if (impl->jack_tunnel) {
		pw_impl_module_destroy(impl->jack_tunnel);
		impl->jack_tunnel = NULL;
	}
}
static void set_started(struct impl *impl, bool started)
{
	if (impl->is_started != started) {
		pw_log_info("New state %d", started);
		impl->is_started = started;
		if (started)
			load_jack_tunnel(impl);
		else
			unload_jack_tunnel(impl);
	}
}

static void impl_free(struct impl *impl)
{
	set_started(impl, false);

	if (impl->bus)
		dbus_connection_unref(impl->bus);
	spa_dbus_connection_destroy(impl->conn);

	pw_properties_free(impl->properties);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void set_pending_call(struct impl *impl, DBusPendingCall *pending)
{
	if (impl->pending_call != NULL) {
		dbus_pending_call_cancel(impl->pending_call);
		dbus_pending_call_unref(impl->pending_call);
	}
	impl->pending_call = pending;
}

static void on_is_started_received(DBusPendingCall *pending,
				   void *user_data)
{
	struct impl *impl = user_data;
	DBusMessage *m;
	DBusError error;
	dbus_bool_t started = false;

	m = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	impl->pending_call = NULL;

	dbus_error_init(&error);

	if (!m) {
		pw_log_error("Failed to receive reply");
		goto error;
	}
	if (dbus_message_is_error(m, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
		pw_log_info("JACK DBus is not running");
		goto error;
	}
	if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *message = "unknown";
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &message, DBUS_TYPE_INVALID);
		pw_log_warn("Failed to receive jackdbus reply: %s: %s",
				dbus_message_get_error_name(m), message);
		goto error;
	}

	dbus_message_get_args(m, &error,
			DBUS_TYPE_BOOLEAN, &started,
			DBUS_TYPE_INVALID);
	dbus_message_unref(m);

	if (dbus_error_is_set(&error)) {
		pw_log_warn("Could not get jackdbus state: %s", error.message);
		goto error;
	}

	pw_log_info("Got jackdbus state %d", started);
	set_started(impl, started);

	return;
error:
	impl->is_started = false;
	dbus_error_free(&error);
}

static void check_jack_running(struct impl *impl)
{
	DBusMessage *m;
	DBusPendingCall *pending;

	impl->is_started = false;

	m = dbus_message_new_method_call(JACK_SERVICE_NAME,
			JACK_INTERFACE_PATH,
			JACK_INTERFACE_NAME,
			"IsStarted");

	dbus_connection_send_with_reply(impl->bus, m, &pending, -1);
	dbus_pending_call_set_notify(pending, on_is_started_received, impl, NULL);

	set_pending_call(impl, pending);
}

static DBusHandlerResult filter_handler(DBusConnection *connection,
		DBusMessage *message, void *user_data)
{
	struct impl *impl = user_data;
	DBusError error;

	dbus_error_init(&error);

	if (dbus_message_is_signal(message, "org.freedesktop.DBus",
				   "NameOwnerChanged")) {
		const char *name, *old, *new;
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_STRING, &old,
					   DBUS_TYPE_STRING, &new,
					   DBUS_TYPE_INVALID)) {
			pw_log_error("Failed to get OwnerChanged args: %s",
					error.message);
			goto not_handled;
		}
		if (!spa_streq(name, JACK_SERVICE_NAME))
			goto not_handled;

		pw_log_info("NameOwnerChanged %s -> %s", old, new);
		if (spa_streq(new, "")) {
			set_pending_call(impl, NULL);
			set_started(impl, false);
		} else {
			check_jack_running(impl);
		}
	}
	else if (dbus_message_is_signal(message, JACK_INTERFACE_NAME,
				   "ServerStarted")) {
		pw_log_info("ServerStarted");
		set_started(impl, true);
	}
	else if (dbus_message_is_signal(message, JACK_INTERFACE_NAME,
				   "ServerStopped")) {
		pw_log_info("ServerStopped");
		set_started(impl, false);
	}
	return DBUS_HANDLER_RESULT_HANDLED;

not_handled:
	dbus_error_free(&error);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int init_dbus_connection(struct impl *impl)
{
	DBusError error;

	impl->bus = spa_dbus_connection_get(impl->conn);
	if (impl->bus == NULL)
		return -EIO;

	dbus_error_init(&error);

	/* XXX: we don't handle dbus reconnection yet, so ref the handle instead */
	dbus_connection_ref(impl->bus);

	dbus_connection_add_filter(impl->bus, filter_handler, impl, NULL);

	dbus_bus_add_match(impl->bus,
			"type='signal',"
			"sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',"
			"member='NameOwnerChanged'", &error);
	if (dbus_error_is_set(&error))
		goto error;

	dbus_bus_add_match(impl->bus,
			"type='signal',"
			"sender='" JACK_SERVICE_NAME "',"
			"interface='" JACK_INTERFACE_NAME "',"
			"member='ServerStarted'", &error);
	if (dbus_error_is_set(&error))
		goto error;

	dbus_bus_add_match(impl->bus,
			"type='signal',"
			"sender='" JACK_SERVICE_NAME "',"
			"interface='" JACK_INTERFACE_NAME "',"
			"member='ServerStopped'", &error);
	if (dbus_error_is_set(&error))
		goto error;

	check_jack_running(impl);

	return 0;
error:
	pw_log_error("Failed to add listener: %s", error.message);
	dbus_error_free(&error);
	return -EIO;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct spa_dbus *dbus;
	const struct spa_support *support;
	uint32_t n_support;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	support = pw_context_get_support(context, &n_support);

	dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
        if (dbus == NULL)
                return -ENOTSUP;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->context = context;
	impl->properties = args ? pw_properties_new_string(args) : NULL;

	impl->conn = spa_dbus_get_connection(dbus, SPA_DBUS_TYPE_SESSION);
	if (impl->conn == NULL) {
		res = -errno;
		goto error;
	}

	if ((res = init_dbus_connection(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;

      error:
	impl_free(impl);
	pw_log_error("Failed to connect to session bus: %s", spa_strerror(res));
	return res;
}
