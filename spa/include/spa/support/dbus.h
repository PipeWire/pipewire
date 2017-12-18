/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_DBUS_H__
#define __SPA_DBUS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <dbus/dbus.h>

#include <spa/support/loop.h>

#define SPA_TYPE__DBus		SPA_TYPE_INTERFACE_BASE "DBus"
#define SPA_TYPE_DBUS_BASE	SPA_TYPE__DBus ":"

#define SPA_TYPE_DBUS__Connection	SPA_TYPE_DBUS_BASE "Connection"

struct spa_dbus_connection {
#define SPA_VERSION_DBUS_CONNECTION	0
        uint32_t version;
	/**
	 * Get the DBusConnection from a wraper
	 *
	 * \param conn the spa_dbus_connection wrapper
	 * \return a DBusConnection
	 */
	DBusConnection *(*get) (struct spa_dbus_connection *conn);
	/**
	 * Destroy a dbus connection wrapper
	 *
	 * \param conn the wrapper to destroy
	 */
	void (*destroy) (struct spa_dbus_connection *conn);
};

#define spa_dbus_connection_get(c)	(c)->get((c))
#define spa_dbus_connection_destroy(c)	(c)->destroy((c))

struct spa_dbus {
	/* the version of this structure. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_DBUS	0
        uint32_t version;

	/**
	 * Get a new connection wrapper for the given bus type.
	 *
	 * The connection wrapper is completely configured to operate
	 * in the main context of the handle that manages the spa_dbus
	 * interface.
	 *
	 * \param dbus the dbus manager
	 * \param type the bus type to wrap
	 * \param error location for the error
	 * \return a new dbus connection wrapper or NULL and \a error is
	 *         set.
	 */
	struct spa_dbus_connection * (*get_connection) (struct spa_dbus *dbus,
							DBusBusType type,
							DBusError *error);
};

#define spa_dbus_get_connection(d,...)	(d)->get_connection((d),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DBUS_H__ */
