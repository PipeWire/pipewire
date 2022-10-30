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

#ifndef SPA_DBUS_MANAGER_H_
#define SPA_DBUS_MANAGER_H_

#include <dbus/dbus.h>

#include <spa/utils/list.h>
#include <spa/support/log.h>

struct spa_dbus_local_object;

/** DBus object manager */
struct spa_dbus_object_manager
{
	/** Root DBus object path */
	const char *path;

	/** List (non-mutable) of objects. */
	struct spa_list object_list;
};

/** DBus property specification a local DBus object. */
struct spa_dbus_property
{
	const char *name;		/**< Name of property */
	const char *signature;		/**< DBus type signature of the value */

	/**
	 * Hook to append bare DBus value to the iterator.
	 * If NULL, the property is considered to not be readable.
	 */
	int (*get)(struct spa_dbus_local_object *object, DBusMessageIter *value);

	/**
	 * Hook to get DBus value from the iterator, and apply it.
	 * If NULL, the property is considered read-only.
	 */
	int (*set)(struct spa_dbus_local_object *object, DBusMessageIter *value);

	/**
	 * Hook to check if the property currently exists.
	 * If NULL, the property always exists.
	 */
	bool (*exists)(struct spa_dbus_local_object *object);
};

/** DBus method specification for a local DBus object. */
struct spa_dbus_method
{
	const char *name;		/**< Name of method */

	/** Hook to react and reply to DBus method call */
	DBusMessage *(*call)(struct spa_dbus_local_object *object, DBusMessage *m);
};

/** DBus interface specification for a local DBus object. */
struct spa_dbus_local_interface
{
	/** Name of the DBus interface */
	const char *name;

	/** Array of properties, zero-terminated */
	const struct spa_dbus_property *properties;

	/** Array of methods, zero-terminated */
	const struct spa_dbus_method *methods;

	/**
	 * Hook called when initializing the object, before
	 * calling any other hooks.
	 */
	void (*init)(struct spa_dbus_local_object *object);

	/**
	 * Hook called once when interface is destroyed.
	 * No other hooks are called after this.
	 */
	void (*destroy)(struct spa_dbus_local_object *object);
};

/**
 * DBus local object structure.
 *
 * One object struct exists for each registered object path.  The same object
 * struct may have multiple interfaces.  The object structures are owned,
 * allocated and freed by the object manager.
 *
 * A custom object struct can also be used, for example
 *
 *     struct my_local_object {
 *         struct spa_dbus_local_object object;
 *         int my_extra_value;
 *     };
 *
 * Its initialization and teardown can be done via the interface
 * init/destroy hooks. Note that the hooks of all interfaces
 * the object has are called on the same object struct.
 *
 * The struct size is specified in the call to
 * spa_dbus_object_manager_register.
 */
struct spa_dbus_local_object
{
	struct spa_list link;		/**< Link (non-mutable) to manager object list */
	const char *path;		/**< DBus object path */

	/** Zero-terminated array of the DBus interfaces of the objects */
	const struct spa_dbus_local_interface *interfaces;

	/** Pointer passed to spa_dbus_object_manager_register */
	void *user_data;
};

/**
 * Create and register new DBus object manager at the given object path.
 *
 * Registers a DBus object with the object manager interface at the given path.
 *
 * \param conn	DBus connection.
 * \param path	Object path to register the new manager at.
 * \param log	Logging output.
 */
struct spa_dbus_object_manager *spa_dbus_object_manager_new(DBusConnection *conn, const char *path, struct spa_log *log);

/** Destroy and unregister the object manager and all objects owned by it. */
void spa_dbus_object_manager_destroy(struct spa_dbus_object_manager *manager);

/**
 * Create and register a new DBus object under the object manager.
 *
 * The DBus object path must be a sub-path of the object manager path.
 *
 * \param manager	Manager that owns the new object.
 * \param path		DBus object path.
 * \param interfaces	Zero-terminated array of interfaces for the new object.
 * \param object_size	Size of the object struct. Must be >= sizeof(struct spa_dbus_local_object).
 * \param user_data	User data pointer to set in the object.
 */
struct spa_dbus_local_object *spa_dbus_object_manager_register(struct spa_dbus_object_manager *manager,
		const char *path,
		const struct spa_dbus_local_interface *interfaces,
		size_t object_size, void *user_data);

/** Find previously registered local DBus object by object path */
struct spa_dbus_local_object *spa_dbus_object_manager_find(struct spa_dbus_object_manager *manager,
		const char *path);

/** Unregister and destroy a previously registered local DBus object */
void spa_dbus_object_manager_unregister(struct spa_dbus_object_manager *manager,
		struct spa_dbus_local_object *object);

/**
 * Emit PropertiesChanged signal for a previously registered local DBus object.
 *
 * \param manager	Object manager
 * \param object	The DBus object
 * \param interface	The interface to emit the signal for
 * \param properties	Zero-terminated array of properties that were changed
 */
int spa_dbus_object_manager_properties_changed(struct spa_dbus_object_manager *manager,
		struct spa_dbus_local_object *object,
		const struct spa_dbus_local_interface *interface,
		const struct spa_dbus_property *properties);

#endif
