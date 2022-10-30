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

#ifndef SPA_DBUS_MONITOR_H_
#define SPA_DBUS_MONITOR_H_

#include <dbus/dbus.h>

#include <spa/utils/list.h>
#include <spa/support/log.h>

struct spa_dbus_object;

/**
 * DBus interface specification.
 */
struct spa_dbus_interface {
	/** DBus interface name. */
	const char *name;

	/** Size of object struct. Must be at least sizeof(struct spa_dbus_object). */
	const size_t object_size;

	/** Property value updated. */
	void (*property) (struct spa_dbus_object *object, const char *key, DBusMessageIter *value);

	/** Interface at the object path added, or property updates complete. */
	void (*update) (struct spa_dbus_object *object);

	/**
	 * Interface at the object path removed.
	 * The object will be deallocated after this, so any associated data,
	 * for example in a custom object struct, can be freed in this hook.
	 */
	void (*remove) (struct spa_dbus_object *object);
};

/**
 * DBus object instance, for one interface.
 *
 * A custom object struct can be also used for each interface, specified
 * as
 *
 *     struct my_dbus_object {
 *         struct spa_dbus_object object;
 *         int some_extra_value;
 *     }
 *
 * The struct will be zero-initialized when first allocated. The object
 * instances are owned by spa_dbus_monitor and allocated and freed by it.
 */
struct spa_dbus_object
{
	struct spa_list link;	/**< Link in interface's object list */
	const char *path;	/**< DBus object path */
	const struct spa_dbus_interface *interface;	/**< The interface of the object */
	void *user_data;	/**< Pointer passed in spa_dbus_monitor_new */
};

/** DBus object monitor */
struct spa_dbus_monitor
{
	const char *service;	/**< Service name */
	const char *path;	/**< Object path */
	const struct spa_dbus_interface *interfaces;	/**< Monitored interfaces
							 * (zero-terminated) */
	void *user_data;	/**< Pointer passed in spa_dbus_monitor_new */
};

/**
 * Non-DBus interface type representing the monitored service itself.
 * Can be used to track NameOwner events.
 */
#define SPA_DBUS_MONITOR_NAME_OWNER_INTERFACE	"org.freedesktop.pipewire.spa.dbus.monitor.owner"

/**
 * Create new object monitor.
 *
 * \param conn DBus connection
 * \param service DBus service name to monitor
 * \param path Object path to monitor
 * \param interfaces Zero-terminated array of interfaces to monitor. Corresponding objects
 *                   will be created.
 * \param log Log to output to
 * \param user_data User data to set in object instances.
 */
struct spa_dbus_monitor *spa_dbus_monitor_new(DBusConnection *conn,
		const char *service,
		const char *path,
		const struct spa_dbus_interface *interfaces,
		struct spa_log *log,
		void *user_data);

/**
 * Destroy object monitor and all interface objects owned by it.
 */
void spa_dbus_monitor_destroy(struct spa_dbus_monitor *monitor);

/** Find interface object by name */
struct spa_dbus_object *spa_dbus_monitor_find(struct spa_dbus_monitor *monitor,
		const char *path, const char *interface);

/** Destroy an object, and don't receive further updates concerning it. */
void spa_dbus_monitor_ignore_object(struct spa_dbus_monitor *monitor,
		struct spa_dbus_object *object);

/**
 * Get the object list for the named interface.
 *
 * The returned list is not mutable.
 *
 * \param monitor The DBus monitor.
 * \param interface Interface name. Must be one of those monitored.
 */
struct spa_list *spa_dbus_monitor_object_list(struct spa_dbus_monitor *monitor,
		const char *interface);

#endif
