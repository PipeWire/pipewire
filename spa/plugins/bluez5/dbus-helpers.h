/* Spa Bluez5 DBus helpers */
/* SPDX-FileCopyrightText: Copyright © 2023 PipeWire authors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_DBUS_HELPERS_H
#define SPA_BLUEZ5_DBUS_HELPERS_H

#include <stdbool.h>

#include <dbus/dbus.h>

#include <spa/utils/cleanup.h>

static inline void cancel_and_unref(DBusPendingCall **pp)
{
	DBusPendingCall *pending_call = spa_steal_ptr(*pp);

	if (pending_call) {
		dbus_pending_call_cancel(pending_call);
		dbus_pending_call_unref(pending_call);
	}
}

static inline DBusMessage *steal_reply_and_unref(DBusPendingCall **pp)
{
	DBusPendingCall *pending_call = spa_steal_ptr(*pp);

	DBusMessage *reply = dbus_pending_call_steal_reply(pending_call);
	dbus_pending_call_unref(pending_call);

	return reply;
}

SPA_DEFINE_AUTOPTR_CLEANUP(DBusMessage, DBusMessage, {
	spa_clear_ptr(*thing, dbus_message_unref);
})

static inline bool reply_with_error(DBusConnection *conn,
				    DBusMessage *reply_to,
				    const char *error_name, const char *error_message)
{
	spa_autoptr(DBusMessage) reply = dbus_message_new_error(reply_to, error_name, error_message);

	return reply && dbus_connection_send(conn, reply, NULL);
}

#endif /* SPA_BLUEZ5_DBUS_HELPERS_H */
