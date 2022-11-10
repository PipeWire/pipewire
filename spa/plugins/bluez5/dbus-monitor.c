/* Spa dbus
 *
 * Copyright © 2022 Pauli Virtanen
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

#include <errno.h>
#include <stddef.h>
#include <stdalign.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/utils/hook.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>

#include "dbus-monitor.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT (&impl->log_topic)

#define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"

struct impl
{
	struct spa_dbus_monitor this;

	DBusConnection *conn;

	struct spa_log_topic log_topic;
	struct spa_log *log;

	struct spa_dbus_async_call get_managed_objects_call;

	unsigned int objects_listed:1;

	struct spa_list objects[];
};

struct object
{
	unsigned int removed:1;
	unsigned int updating:1;
	struct spa_dbus_object alignas(max_align_t) this;
};

static void object_emit_update(struct object *o)
{
	if (o->this.interface->update)
		o->this.interface->update(&o->this);
}

static void object_emit_remove(struct object *o)
{
	if (o->this.interface->remove)
		o->this.interface->remove(&o->this);
}

static void object_emit_property(struct object *o, const char *key, DBusMessageIter *value)
{
	if (o->this.interface->property)
		o->this.interface->property(&o->this, key, value);
}

static struct object *object_new(struct impl *impl, const struct spa_dbus_interface *iface,
		struct spa_list *object_list, const char *path)
{
	struct object *o;

	spa_assert(iface->object_size >= sizeof(struct spa_dbus_object));

	o = calloc(1, sizeof(struct object) + iface->object_size - sizeof(struct spa_dbus_object));
	if (o == NULL)
		return NULL;

	o->this.path = strdup(path);
	o->this.interface = iface;
	o->this.user_data = impl->this.user_data;

	spa_assert(o->this.path);

	spa_list_append(object_list, &o->this.link);

	return o;
}

static void object_remove(struct object *o)
{
	if (o->removed)
		return;

	o->updating = true;
	o->removed = true;
	object_emit_remove(o);
	spa_list_remove(&o->this.link);
}

static void object_destroy(struct object *o)
{
	object_remove(o);
	free((void *)o->this.path);
	free(o);
}

static const struct spa_dbus_interface *interface_find(struct impl *impl, const char *interface, struct spa_list **objects)
{
	const struct spa_dbus_interface *iface;
	size_t i;

	for (iface = impl->this.interfaces, i = 0; iface && iface->name; ++iface, ++i) {
		if (spa_streq(iface->name, interface)) {
			*objects = &impl->objects[i];
			return iface;
		}
	}

	return NULL;
}

static struct object *object_find(struct spa_list *object_list, const char *path)
{
	struct spa_dbus_object *object;

	spa_list_for_each(object, object_list, link) {
		struct object *o = SPA_CONTAINER_OF(object, struct object, this);

		if (spa_streq(o->this.path, path))
			return o;
	}

	return NULL;
}

static int object_update_props(struct impl *impl,
		struct object *o,
		DBusMessageIter *props_iter,
		DBusMessageIter *invalidated_iter)
{
	o->updating = true;

	if (o->this.interface->property) {
		while (invalidated_iter && dbus_message_iter_get_arg_type(invalidated_iter) != DBUS_TYPE_INVALID) {
			const char *key;

			dbus_message_iter_get_basic(invalidated_iter, &key);

			object_emit_property(o, key, NULL);
			if (o->removed)
				goto removed;

			dbus_message_iter_next(invalidated_iter);
		}

		while (props_iter && dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
			DBusMessageIter entry, value;
			const char *key;

			dbus_message_iter_recurse(props_iter, &entry);
			dbus_message_iter_get_basic(&entry, &key);
			dbus_message_iter_next(&entry);
			dbus_message_iter_recurse(&entry, &value);

			object_emit_property(o, key, &value);
			if (o->removed)
				goto removed;

			dbus_message_iter_next(props_iter);
		}
	}

	object_emit_update(o);
	if (o->removed)
		goto removed;

	o->updating = false;
	return 0;

removed:
	object_destroy(o);
	return 0;
}

static void interface_added(struct impl *impl,
		const char *object_path,
		const char *interface_name,
		DBusMessageIter *props_iter)
{
	struct object *o;
	const struct spa_dbus_interface *iface;
	struct spa_list *object_list;

	iface = interface_find(impl, interface_name, &object_list);
	if (!iface) {
		spa_log_trace(impl->log, "dbus: skip path=%s, interface=%s",
				object_path, interface_name);
		return;
	}

	spa_log_debug(impl->log, "dbus: added path=%s, interface=%s", object_path, interface_name);

	o = object_find(object_list, object_path);
	if (o == NULL) {
		o = object_new(impl, iface, object_list, object_path);
		if (o == NULL) {
			spa_log_warn(impl->log, "can't create object: %m");
			return;
		}

	}

	object_update_props(impl, o, props_iter, NULL);
}

static void interfaces_added(struct impl *impl, DBusMessageIter *arg_iter)
{
	DBusMessageIter it[3];
	const char *object_path;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it[0]);

	while (dbus_message_iter_get_arg_type(&it[0]) != DBUS_TYPE_INVALID) {
		const char *interface_name;

		dbus_message_iter_recurse(&it[0], &it[1]);
		dbus_message_iter_get_basic(&it[1], &interface_name);
		dbus_message_iter_next(&it[1]);
		dbus_message_iter_recurse(&it[1], &it[2]);

		interface_added(impl, object_path, interface_name, &it[2]);

		dbus_message_iter_next(&it[0]);
	}
}

static void interfaces_removed(struct impl *impl, DBusMessageIter *arg_iter)
{
	const char *object_path;
	DBusMessageIter it;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it);

	while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
		const char *interface_name;
		const struct spa_dbus_interface *iface;
		struct spa_list *object_list;

		dbus_message_iter_get_basic(&it, &interface_name);

		iface = interface_find(impl, interface_name, &object_list);
		if (iface) {
			struct object *o;

			spa_log_debug(impl->log, "dbus: removed path=%s, interface=%s",
					object_path, interface_name);

			o = object_find(object_list, object_path);
			if (o)
				object_destroy(o);
		} else {
			spa_log_trace(impl->log, "dbus: skip removed path=%s, interface=%s",
					object_path, interface_name);
		}

		dbus_message_iter_next(&it);
	}
}

static void get_managed_objects_reply(struct spa_dbus_async_call *call, DBusMessage *r)
{
	struct impl *impl = SPA_CONTAINER_OF(call, struct impl, get_managed_objects_call);
	DBusMessageIter it[6];

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(impl->log, "ObjectManager not available at path=%s",
			impl->this.path);
		return;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(impl->log, "GetManagedObjects() at %s failed: %s",
				impl->this.path, dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &it[0]) ||
			!dbus_message_has_signature(r, "a{oa{sa{sv}}}")) {
		spa_log_error(impl->log, "Invalid reply signature for GetManagedObjects() at %s",
				impl->this.path);
		return;
	}

	/* Add fake object representing the service itself */
	interface_added(impl, impl->this.path, SPA_DBUS_MONITOR_NAME_OWNER_INTERFACE, NULL);

	dbus_message_iter_recurse(&it[0], &it[1]);

	while (dbus_message_iter_get_arg_type(&it[1]) != DBUS_TYPE_INVALID) {
		dbus_message_iter_recurse(&it[1], &it[2]);

		interfaces_added(impl, &it[2]);

		dbus_message_iter_next(&it[1]);
	}

	impl->objects_listed = true;
}

static int get_managed_objects(struct impl *impl)
{
	DBusMessage *m;

	if (impl->objects_listed || impl->get_managed_objects_call.pending)
		return 0;

	m = dbus_message_new_method_call(impl->this.service,
			impl->this.path,
			DBUS_INTERFACE_OBJECT_MANAGER,
			"GetManagedObjects");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_set_auto_start(m, false);

	return spa_dbus_async_call_send(&impl->get_managed_objects_call, impl->conn, m,
			get_managed_objects_reply);
}


static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct impl *impl = user_data;
	DBusError err = DBUS_ERROR_INIT;

	if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;
		bool has_old_owner, has_new_owner;

		spa_log_debug(impl->log, "dbus: name owner changed %s", dbus_message_get_path(m));

		if (!dbus_message_get_args(m, &err,
						DBUS_TYPE_STRING, &name,
						DBUS_TYPE_STRING, &old_owner,
						DBUS_TYPE_STRING, &new_owner,
						DBUS_TYPE_INVALID)) {
			spa_log_error(impl->log,
					"Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s",
					err.message);
			goto done;
		}

		if (!spa_streq(name, impl->this.service))
			goto done;

		has_old_owner = old_owner && *old_owner;
		has_new_owner = new_owner && *new_owner;

		if (has_old_owner)
			spa_log_debug(impl->log, "dbus: %s disappeared", impl->this.service);

		if (has_old_owner || has_new_owner) {
			size_t i;
			const struct spa_dbus_interface *iface;

			impl->objects_listed = false;

			for (iface = impl->this.interfaces, i = 0; iface && iface->name; ++iface, ++i) {
				struct spa_dbus_object *object;

				spa_list_consume(object, &impl->objects[i], link) {
					struct object *o = SPA_CONTAINER_OF(object, struct object, this);

					object_destroy(o);
				}
			}
		}

		if (has_new_owner) {
			spa_log_debug(impl->log, "dbus: %s appeared", impl->this.service);
			get_managed_objects(impl);
		}
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded")) {
		DBusMessageIter it;

		spa_log_debug(impl->log, "dbus: interfaces added on path=%s", dbus_message_get_path(m));

		if (!impl->objects_listed)
			goto done;

		if (!dbus_message_iter_init(m, &it) || !dbus_message_has_signature(m, "oa{sa{sv}}")) {
			spa_log_error(impl->log, "Invalid signature found in InterfacesAdded");
			goto done;
		}

		interfaces_added(impl, &it);
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved")) {
		DBusMessageIter it;

		spa_log_debug(impl->log, "dbus: interfaces removed on path=%s", dbus_message_get_path(m));

		if (!impl->objects_listed)
			goto done;

		if (!dbus_message_iter_init(m, &it) || !dbus_message_has_signature(m, "oas")) {
			spa_log_error(impl->log, "Invalid signature found in InterfacesRemoved");
			goto done;
		}

		interfaces_removed(impl, &it);
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged")) {
		DBusMessageIter args, props, invalidated;
		const char *interface_name, *path;
		const struct spa_dbus_interface *iface;
		struct spa_list *object_list;

		if (!impl->objects_listed)
			goto done;

		if (!dbus_message_iter_init(m, &args) ||
				!dbus_message_has_signature(m, "sa{sv}as")) {
			spa_log_error(impl->log, "Invalid signature found in PropertiesChanged");
			goto done;
		}
		path = dbus_message_get_path(m);

		dbus_message_iter_get_basic(&args, &interface_name);
		dbus_message_iter_next(&args);
		dbus_message_iter_recurse(&args, &props);
		dbus_message_iter_next(&args);
		dbus_message_iter_recurse(&args, &invalidated);

		iface = interface_find(impl, interface_name, &object_list);
		if (iface) {
			struct object *o;

			o = object_find(object_list, path);
			if (o == NULL) {
				spa_log_debug(impl->log, "Properties changed in unknown object %s",
						path);
				goto done;
			}

			spa_log_debug(impl->log, "dbus: properties changed in path=%s", path);
			object_update_props(impl, o, &props, &invalidated);
		}
	}

done:
	dbus_error_free(&err);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_filters(struct impl *impl)
{
	DBusError err = DBUS_ERROR_INIT;
	char rule[1024];
	const struct spa_dbus_interface *iface;

	if (!dbus_connection_add_filter(impl->conn, filter_cb, impl, NULL)) {
		spa_log_error(impl->log, "failed to add DBus filter");
		return -EIO;
	}

	spa_scnprintf(rule, sizeof(rule),
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='%s'",
			impl->this.service);
	spa_log_trace(impl->log, "add match: %s", rule);
	dbus_bus_add_match(impl->conn, rule, &err);
	if (dbus_error_is_set(&err))
		goto fail;

	spa_scnprintf(rule, sizeof(rule),
			"type='signal',sender='%s',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'",
			impl->this.service);
	spa_log_trace(impl->log, "add match: %s", rule);
	dbus_bus_add_match(impl->conn, rule, &err);
	if (dbus_error_is_set(&err))
		goto fail;

	spa_scnprintf(rule, sizeof(rule),
			"type='signal',sender='%s',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'",
			impl->this.service);
	spa_log_trace(impl->log, "add match: %s", rule);
	dbus_bus_add_match(impl->conn, rule, &err);
	if (dbus_error_is_set(&err))
		goto fail;

	for (iface = impl->this.interfaces; iface && iface->name; ++iface) {
		spa_scnprintf(rule, sizeof(rule),
				"type='signal',sender='%s',"
				"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
				"arg0='%s'",
				impl->this.service, iface->name);
		spa_log_trace(impl->log, "add match: %s", rule);
		dbus_bus_add_match(impl->conn, rule, &err);
		if (dbus_error_is_set(&err))
			goto fail;
	}

	dbus_error_free(&err);
	return 0;

fail:
	dbus_connection_remove_filter(impl->conn, filter_cb, impl);

	spa_log_error(impl->log, "failed to add DBus match: %s", err.message);
	dbus_error_free(&err);
	return -EIO;
}

struct spa_dbus_monitor *spa_dbus_monitor_new(DBusConnection *conn,
		const char *service,
		const char *path,
		const struct spa_dbus_interface *interfaces,
		struct spa_log *log,
		void *user_data)
{
	struct impl *impl;
	const struct spa_dbus_interface *iface;
	size_t num_interfaces = 0, i;
	int res;

	for (iface = interfaces; iface && iface->name; ++iface) {
		spa_assert(iface->object_size >= sizeof(struct spa_dbus_object));
		++num_interfaces;
	}

	impl = calloc(1, sizeof(struct impl) + sizeof(struct spa_list) * num_interfaces);
	if (impl == NULL)
		return NULL;

	impl->conn = conn;
	impl->this.service = strdup(service);
	impl->this.path = strdup(path);
	impl->this.interfaces = interfaces;
	impl->this.user_data = user_data;

	impl->log = log;

	impl->log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.dbus");
	spa_log_topic_init(impl->log, &impl->log_topic);

	for (i = 0; i < num_interfaces; ++i)
		spa_list_init(&impl->objects[i]);

	spa_assert(impl->this.service);
	spa_assert(impl->this.path);

	if ((res = add_filters(impl)) < 0) {
		free((void *)impl->this.service);
		free((void *)impl->this.path);
		free(impl);
		errno = -res;
		return NULL;
	}

	dbus_connection_ref(conn);

	get_managed_objects(impl);

	return &impl->this;
}

void spa_dbus_monitor_destroy(struct spa_dbus_monitor *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	const struct spa_dbus_interface *iface;
	struct spa_dbus_object *object;
	size_t i;

	dbus_connection_remove_filter(impl->conn, filter_cb, impl);

	spa_dbus_async_call_cancel(&impl->get_managed_objects_call);

	for (iface = impl->this.interfaces, i = 0; iface && iface->name; ++iface, ++i) {
		spa_list_consume(object, &impl->objects[i], link) {
			struct object *o = SPA_CONTAINER_OF(object, struct object, this);

			object_destroy(o);
		}
	}

	dbus_connection_unref(impl->conn);

	free((void *)impl->this.service);
	free((void *)impl->this.path);
	free(impl);
}

struct spa_dbus_object *spa_dbus_monitor_find(struct spa_dbus_monitor *this, const char *path, const char *interface)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct object *o;
	const struct spa_dbus_interface *iface;
	struct spa_list *object_list;

	if (path == NULL)
		return NULL;

	spa_assert(interface);

	iface = interface_find(impl, interface, &object_list);
	if (!iface)
		return NULL;

	o = object_find(object_list, path);
	if (o)
		return &o->this;

	return NULL;
}

void spa_dbus_monitor_ignore_object(struct spa_dbus_monitor *this, struct spa_dbus_object *object)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct object *o = SPA_CONTAINER_OF(object, struct object, this);

	spa_log_trace(impl->log, "ignore path=%s", o->this.path);

	if (o->updating)
		object_remove(o);
	else
		object_destroy(o);
}

struct spa_list *spa_dbus_monitor_object_list(struct spa_dbus_monitor *this, const char *interface)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	const struct spa_dbus_interface *iface;
	struct spa_list *object_list;

	iface = interface_find(impl, interface, &object_list);
	if (!iface)
		return NULL;

	return object_list;
}

static void async_call_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_dbus_async_call *call = user_data;
	DBusMessage *r;

	spa_assert(pending == call->pending);
	call->pending = NULL;

	r = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);

	spa_assert(call->reply != NULL);

	if (r == NULL)
		return;

	call->reply(call, r);
	dbus_message_unref(r);
}

int spa_dbus_async_call_send(struct spa_dbus_async_call *call,
		DBusConnection *conn, DBusMessage *msg,
		void (*reply)(struct spa_dbus_async_call *call, DBusMessage *reply))
{
	int res = -EIO;
	DBusPendingCall *pending;

	spa_assert(msg);
	spa_assert(conn);
	spa_assert(call);

	if (call->pending) {
		res = -EBUSY;
		goto done;
	}

	if (!dbus_connection_send_with_reply(conn, msg, &pending, -1))
		goto done;

	if (!dbus_pending_call_set_notify(pending, async_call_reply, call, NULL)) {
		dbus_pending_call_cancel(pending);
		dbus_pending_call_unref(pending);
		goto done;
	}

	call->reply = reply;
	call->pending = pending;
	res = 0;

done:
	dbus_message_unref(msg);
	return res;
}
