/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <stddef.h>

#include <pipewire/keys.h>

#include "remap.h"

const struct str_map media_role_map[] = {
	{ "Movie", "video", },
	{ "Music", "music", },
	{ "Game", "game", },
	{ "Notification", "event", },
	{ "Communication", "phone", },
	{ "Movie", "animation", },
	{ "Production", "production", },
	{ "Accessibility", "a11y", },
	{ "Test", "test", },
	{ NULL, NULL },
};

const struct str_map props_key_map[] = {
	{ PW_KEY_DEVICE_BUS_PATH, "device.bus_path" },
	{ PW_KEY_DEVICE_FORM_FACTOR, "device.form_factor" },
	{ PW_KEY_DEVICE_ICON_NAME, "device.icon_name" },
	{ PW_KEY_DEVICE_INTENDED_ROLES, "device.intended_roles" },
	{ PW_KEY_NODE_DESCRIPTION, "device.description" },
	{ PW_KEY_MEDIA_ICON_NAME, "media.icon_name" },
	{ PW_KEY_APP_ICON_NAME, "application.icon_name" },
	{ PW_KEY_APP_PROCESS_MACHINE_ID, "application.process.machine_id" },
	{ PW_KEY_APP_PROCESS_SESSION_ID, "application.process.session_id" },
	{ PW_KEY_MEDIA_ROLE, "media.role", media_role_map },
	{ NULL, NULL },
};
