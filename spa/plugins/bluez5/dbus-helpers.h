/* Spa Bluez5 DBus helpers */
/* SPDX-FileCopyrightText: Copyright © 2023 PipeWire authors */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_DBUS_HELPERS_H
#define SPA_BLUEZ5_DBUS_HELPERS_H

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

#endif /* SPA_BLUEZ5_DBUS_HELPERS_H */
