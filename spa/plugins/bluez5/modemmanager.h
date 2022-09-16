/* Spa Bluez5 ModemManager proxy
 *
 * Copyright © 2022 Collabora
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

#ifndef SPA_BLUEZ5_MODEMMANAGER_H_
#define SPA_BLUEZ5_MODEMMANAGER_H_

#include "defs.h"

struct mm_ops {
	void (*set_modem_service)(bool available, void *user_data);
	void (*set_modem_signal_strength)(unsigned int strength, void *user_data);
};

#ifdef HAVE_BLUEZ_5_BACKEND_NATIVE_MM
void *mm_register(struct spa_log *log, void *dbus_connection, const struct mm_ops *ops, void *user_data);
void mm_unregister(void *data);
#else
void *mm_register(struct spa_log *log, void *dbus_connection, const struct mm_ops *ops, void *user_data)
{
	return NULL;
}

void mm_unregister(void *data)
{
}
#endif

#endif
