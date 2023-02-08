/* Spa Bluez5 UPower proxy */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_UPOWER_H_
#define SPA_BLUEZ5_UPOWER_H_

#include "defs.h"

void *upower_register(struct spa_log *log,
                      void *dbus_connection,
                      void (*set_battery_level)(unsigned int level, void *user_data),
                      void *user_data);
void upower_unregister(void *data);

#endif
