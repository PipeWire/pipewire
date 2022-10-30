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

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/utils/hook.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>

#include "dbus-manager.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT (&impl->log_topic)

#define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"

struct object;

struct impl
{
	struct spa_dbus_object_manager this;

	DBusConnection *conn;

	struct spa_log_topic log_topic;
	struct spa_log *log;

	struct object *root;
};

struct object
{
	struct impl *impl;
	union {
		struct spa_dbus_local_object this;
		/* extra data follows 'this', force safer alignment */
		long double _align;
	};
};

const struct spa_dbus_property *object_interface_get_property(struct object *o,
		const struct spa_dbus_local_interface *iface, const char *name)
{
	const struct spa_dbus_property *prop;

	for (prop = iface->properties; prop && prop->name; ++prop) {
		if (spa_streq(prop->name, name) && (prop->exists == NULL || prop->exists(&o->this)))
			return prop;
	}

	return NULL;
}

const struct spa_dbus_local_interface *object_get_interface(struct object *o, const char *interface)
{
	const struct spa_dbus_local_interface *iface;

	for (iface = o->this.interfaces; iface && iface->name; ++iface)
		if (spa_streq(interface, iface->name))
			return iface;

	return NULL;
}

static DBusMessage *object_properties_get(struct object *o, DBusMessage *m)
{
	struct impl *impl = o->impl;
	const char *interface, *name;
	const struct spa_dbus_local_interface *iface;
	const struct spa_dbus_property *prop;
	DBusMessage *r;
	DBusMessageIter i, v;
	int res;

	if (!dbus_message_get_args(m, NULL,
					DBUS_TYPE_STRING, &interface,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"Invalid arguments");

	iface = object_get_interface(o, interface);
	if (!iface)
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");

	prop = object_interface_get_property(o, iface, name);
	if (!prop)
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_PROPERTY,
				"No such property");

	if (!prop->get)
		return dbus_message_new_error(m, DBUS_ERROR_FAILED,
				"Write-only property");

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT, prop->signature, &v);

	if ((res = prop->get(&o->this, &v)) < 0) {
		spa_log_debug(impl->log, "failed to get property %s value: %s",
				prop->name, spa_strerror(res));
		dbus_message_unref(r);
		return dbus_message_new_error(m, DBUS_ERROR_FAILED,
				"Failed to get property");
	}

	dbus_message_iter_close_container(&i, &v);

	return r;
}

static int object_append_properties(struct object *o, const struct spa_dbus_property *properties, DBusMessageIter *i)
{
	struct impl *impl = o->impl;
	DBusMessageIter d;
	const struct spa_dbus_property *prop;

	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sv}", &d);

	for (prop = properties; prop && prop->name; ++prop) {
		DBusMessageIter e, v;
		int res;

		if (!(prop->exists == NULL || prop->exists(&o->this)) || !prop->get)
			continue;

		dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &prop->name);
		dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, prop->signature, &v);

		if ((res = prop->get(&o->this, &v)) < 0) {
			spa_log_debug(impl->log, "failed to get property %s value: %s",
					prop->name, spa_strerror(res));
			return res;
		}

		dbus_message_iter_close_container(&e, &v);
		dbus_message_iter_close_container(&d, &e);
	}

	dbus_message_iter_close_container(i, &d);

	return 0;
}

static DBusMessage *object_properties_get_all(struct object *o, DBusMessage *m)
{
	const char *interface;
	const struct spa_dbus_local_interface *iface;
	DBusMessageIter i;
	DBusMessage *r;

	if (!dbus_message_get_args(m, NULL,
					DBUS_TYPE_STRING, &interface,
					DBUS_TYPE_INVALID))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"Invalid arguments");

	iface = object_get_interface(o, interface);
	if (!iface)
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);

	if (object_append_properties(o, iface->properties, &i) < 0) {
		dbus_message_unref(r);
		return dbus_message_new_error(m, DBUS_ERROR_FAILED,
				"Failed to get properties");
	}

	return r;
}

static DBusMessage *object_properties_set(struct object *o, DBusMessage *m)
{
	struct impl *impl = o->impl;
	const char *interface, *name;
	const struct spa_dbus_local_interface *iface;
	const struct spa_dbus_property *prop;
	DBusMessageIter it, value;
	char *value_signature;
	bool valid_signature;
	int res;

	if (!dbus_message_has_signature(m, "ssv"))
		return NULL;

	dbus_message_iter_init(m, &it);
	dbus_message_iter_get_basic(&it, &interface);
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &name);
	dbus_message_iter_next(&it);

	iface = object_get_interface(o, interface);
	if (!iface)
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");

	prop = object_interface_get_property(o, iface, name);
	if (!prop)
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_PROPERTY,
				"No such property");

	if (prop->set == NULL)
		return dbus_message_new_error(m, DBUS_ERROR_PROPERTY_READ_ONLY,
				"Read-only property");

	dbus_message_iter_recurse(&it, &value);

	value_signature = dbus_message_iter_get_signature(&value);
	valid_signature = spa_streq(prop->signature, value_signature);
	dbus_free(value_signature);
	if (!valid_signature)
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_SIGNATURE,
				"Invalid value signature");

	if ((res = prop->set(&o->this, &value)) < 0) {
		spa_log_debug(impl->log, "failed to set property %s value: %s",
				prop->name, spa_strerror(res));
		return dbus_message_new_error(m, DBUS_ERROR_FAILED,
				"Failed to set property");
	}

	return dbus_message_new_method_return(m);
}

static DBusHandlerResult object_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct object *o = userdata;
	struct impl *impl = o->impl;
	const char *path, *interface, *member;
	DBusMessage *r = NULL;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(impl->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Get")) {
		r = object_properties_get(o, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
		r = object_properties_get_all(o, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
		r = object_properties_set(o, m);
	} else {
		const struct spa_dbus_local_interface *iface = object_get_interface(o, interface);
		bool called = false;

		if (iface) {
			const struct spa_dbus_method *method;

			for (method = iface->methods; method && method->name; ++method) {
				if (dbus_message_is_method_call(m, iface->name, method->name)) {
					r = method->call(&o->this, m);
					called = true;
					break;
				}
			}
		}

		if (!called)
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r != NULL && dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_HANDLED;
	else if (r)
		dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static int object_signal_interfaces_added(struct object *o)
{
	struct impl *impl = o->impl;
	struct object *root = impl->root;
	const struct spa_dbus_local_interface *iface;
	DBusMessage *s;
	DBusMessageIter i, a;

	if (root == NULL)
		root = o;  /* we're root, impl still initializing */

	s = dbus_message_new_signal(root->this.path,
			DBUS_INTERFACE_OBJECT_MANAGER,
			"InterfacesAdded");
	if (s == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(s, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &o->this.path);

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sa{sv}}", &a);

	for (iface = o->this.interfaces; iface && iface->name; ++iface) {
		DBusMessageIter e;
		int res;

		spa_log_debug(impl->log, "dbus: signal add interface path=%s interface=%s",
				o->this.path, iface->name);

		dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &iface->name);

		if ((res = object_append_properties(o, iface->properties, &e)) < 0) {
			dbus_message_unref(s);
			return res;
		}

		dbus_message_iter_close_container(&a, &e);
	}

	dbus_message_iter_close_container(&i, &a);

	dbus_connection_send(impl->conn, s, NULL);
	dbus_message_unref(s);

	return 0;
}

static int object_signal_interfaces_removed(struct object *o)
{
	struct impl *impl = o->impl;
	struct object *root = impl->root;
	const struct spa_dbus_local_interface *iface;
	DBusMessage *s;
	DBusMessageIter i, a;

	spa_assert(root);

	s = dbus_message_new_signal(root->this.path,
			DBUS_INTERFACE_OBJECT_MANAGER,
			"InterfacesRemoved");
	if (s == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(s, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &o->this.path);

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "s", &a);

	for (iface = o->this.interfaces; iface && iface->name; ++iface) {
		spa_log_debug(impl->log, "dbus: signal remove interface path=%s interface=%s",
				o->this.path, iface->name);

		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &iface->name);
	}

	dbus_message_iter_close_container(&i, &a);

	dbus_connection_send(impl->conn, s, NULL);
	dbus_message_unref(s);

	return 0;
}

static int object_signal_properties_changed(struct object *o,
		const struct spa_dbus_local_interface *iface,
		const struct spa_dbus_property *properties)
{
	struct impl *impl = o->impl;
	const struct spa_dbus_property *prop;
	DBusMessage *s;
	DBusMessageIter i, a;
	int res;

	if (properties == NULL || properties->name == NULL) {
		/* nothing was changed */
		return 0;
	}

	s = dbus_message_new_signal(o->this.path,
			DBUS_INTERFACE_PROPERTIES,
			"PropertiesChanged");
	if (s == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(s, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &iface->name);

	if ((res = object_append_properties(o, properties, &i)) < 0) {
		dbus_message_unref(s);
		return res;
	}

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "s", &a);

	for (prop = properties; prop && prop->name; ++prop) {
		spa_log_debug(impl->log, "dbus: signal properties changed path=%s interface=%s property=%s",
				o->this.path, iface->name, prop->name);

		if (prop->exists == NULL || prop->exists(&o->this))
			continue;
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &prop->name);
	}

	dbus_message_iter_close_container(&i, &a);

	dbus_connection_send(impl->conn, s, NULL);
	dbus_message_unref(s);

	return 0;
}

static struct object *object_new(struct impl *impl,
		const char *path,
		const struct spa_dbus_local_interface *interfaces,
		size_t object_size,
		void *user_data)
{
	static const DBusObjectPathVTable vtable = {
		.message_function = object_handler,
	};
	const struct spa_dbus_local_interface *iface;
	struct object *o;

	o = calloc(1, sizeof(struct object) - sizeof(struct spa_dbus_local_object) + object_size);
	if (o == NULL)
		return NULL;

	spa_log_debug(impl->log, "dbus: register path=%s", path);

	if (!dbus_connection_register_object_path(impl->conn, path, &vtable, o)) {
		free(o);
		errno = EIO;
		return NULL;
	}

	spa_list_append(&impl->this.object_list, &o->this.link);
	o->impl = impl;
	o->this.path = strdup(path);
	o->this.interfaces = interfaces;
	o->this.user_data = user_data;

	spa_assert(o->this.path);

	for (iface = o->this.interfaces; iface && iface->name; ++iface) {
		if (iface->init)
			iface->init(&o->this);
	}

	object_signal_interfaces_added(o);

	return o;
}

static void object_destroy(struct object *o)
{
	struct impl *impl = o->impl;
	const struct spa_dbus_local_interface *iface;

	object_signal_interfaces_removed(o);

	spa_list_remove(&o->this.link);

	for (iface = o->this.interfaces; iface && iface->name; ++iface) {
		if (iface->destroy)
			iface->destroy(&o->this);
	}

	spa_log_debug(impl->log, "dbus: unregister path=%s", o->this.path);

	dbus_connection_unregister_object_path(impl->conn, o->this.path);

	free(o);
}

static struct object *object_find(struct impl *impl, const char *path)
{
	struct spa_dbus_local_object *obj;

	spa_list_for_each(obj, &impl->this.object_list, link) {
		struct object *o = SPA_CONTAINER_OF(obj, struct object, this);

		if (spa_streq(o->this.path, path))
			return o;
	}

	return NULL;
}

static DBusMessage *root_get_managed_objects(struct spa_dbus_local_object *object, DBusMessage *m)
{
	struct impl *impl = object->user_data;
	DBusMessage *r;
	DBusMessageIter i, object_array;
	struct spa_dbus_local_object *obj;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &object_array);

	spa_list_for_each(obj, &impl->this.object_list, link) {
		struct object *o = SPA_CONTAINER_OF(obj, struct object, this);

		const struct spa_dbus_local_interface *iface;
		DBusMessageIter object_entry;
		DBusMessageIter interface_array;

		dbus_message_iter_open_container(&object_array, DBUS_TYPE_DICT_ENTRY, NULL, &object_entry);
		dbus_message_iter_append_basic(&object_entry, DBUS_TYPE_OBJECT_PATH, &o->this.path);

		dbus_message_iter_open_container(&object_entry, DBUS_TYPE_ARRAY, "{sa{sv}}", &interface_array);

		for (iface = o->this.interfaces; iface && iface->name; ++iface) {
			DBusMessageIter interface_entry;

			dbus_message_iter_open_container(&interface_array, DBUS_TYPE_DICT_ENTRY, NULL, &interface_entry);
			dbus_message_iter_append_basic(&interface_entry, DBUS_TYPE_STRING, &iface->name);

			if (object_append_properties(o, iface->properties, &interface_entry) < 0) {
				dbus_message_unref(r);
				return dbus_message_new_error(m, DBUS_ERROR_FAILED,
						"Failed to get properties");
			}

			dbus_message_iter_close_container(&interface_array, &interface_entry);
		}

		dbus_message_iter_close_container(&object_entry, &interface_array);
		dbus_message_iter_close_container(&object_array, &object_entry);
	}

	dbus_message_iter_close_container(&i, &object_array);

	return r;
}

static const struct spa_dbus_method root_methods[] = {
	{
		.name = "GetManagedObjects",
		.call = root_get_managed_objects,
	},
	{NULL}
};

static const struct spa_dbus_local_interface root_interfaces[] = {
	{
		.name = DBUS_INTERFACE_OBJECT_MANAGER,
		.methods = root_methods,
	},
	{NULL}
};

struct spa_dbus_object_manager *spa_dbus_object_manager_new(DBusConnection *conn, const char *path, struct spa_log *log)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->conn = conn;
	impl->log = log;

	impl->log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.dbus");

	spa_log_topic_init(impl->log, &impl->log_topic);

	spa_list_init(&impl->this.object_list);

	impl->root = object_new(impl, path, root_interfaces, sizeof(struct spa_dbus_local_object), impl);
	if (impl->root == NULL) {
		free(impl);
		return NULL;
	}

	impl->this.path = impl->root->this.path;

	dbus_connection_ref(impl->conn);

	return &impl->this;
}

void spa_dbus_object_manager_destroy(struct spa_dbus_object_manager *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct spa_dbus_local_object *obj;
	struct spa_list tmp;

	spa_list_init(&tmp);
	spa_list_remove(&impl->root->this.link);
	spa_list_append(&tmp, &impl->root->this.link);

	spa_list_consume(obj, &impl->this.object_list, link) {
		struct object *o = SPA_CONTAINER_OF(obj, struct object, this);

		object_destroy(o);
	}

	object_destroy(impl->root);

	dbus_connection_unref(impl->conn);
	free(impl);
}

struct spa_dbus_local_object *spa_dbus_object_manager_register(struct spa_dbus_object_manager *this,
		const char *path,
		const struct spa_dbus_local_interface *interfaces,
		size_t object_size, void *user_data)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct object *o;
	int root_len = strlen(this->path);

	if (!(spa_strstartswith(path, this->path) && path[root_len] == '/' &&
					path[root_len + 1] != '\0')) {
		errno = EINVAL;
		return NULL;
	}

	o = object_new(impl, path, interfaces, object_size, user_data);
	if (o == NULL)
		return NULL;

	return &o->this;
}

void spa_dbus_object_manager_unregister(struct spa_dbus_object_manager *this,
		struct spa_dbus_local_object *object)
{
	struct object *o = SPA_CONTAINER_OF(object, struct object, this);

	object_destroy(o);
}

struct spa_dbus_local_object *spa_dbus_object_manager_find(struct spa_dbus_object_manager *this,
		const char *path)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct object *o;

	o = object_find(impl, path);
	if (o)
		return &o->this;

	return NULL;
}


int spa_dbus_object_manager_properties_changed(struct spa_dbus_object_manager *this,
		struct spa_dbus_local_object *object,
		const struct spa_dbus_local_interface *interface,
		const struct spa_dbus_property *properties)
{
	struct object *o = SPA_CONTAINER_OF(object, struct object, this);

	return object_signal_properties_changed(o, interface, properties);
}
