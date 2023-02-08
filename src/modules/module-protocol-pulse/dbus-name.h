/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_DBUS_NAME_H
#define PULSE_SERVER_DBUS_NAME_H

struct pw_context;

void *dbus_request_name(struct pw_context *context, const char *name);
void dbus_release_name(void *data);

#endif /* PULSE_SERVER_DBUS_NAME_H */
