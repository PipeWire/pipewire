/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
	{ PW_KEY_DEVICE_SYSFS_PATH, "sysfs.path" },
	{ PW_KEY_DEVICE_FORM_FACTOR, "device.form_factor" },
	{ PW_KEY_DEVICE_ICON_NAME, "device.icon_name" },
	{ PW_KEY_DEVICE_INTENDED_ROLES, "device.intended_roles" },
	{ PW_KEY_NODE_DESCRIPTION, "device.description" },
	{ PW_KEY_MEDIA_ICON_NAME, "media.icon_name" },
	{ PW_KEY_APP_ICON_NAME, "application.icon_name" },
	{ PW_KEY_APP_PROCESS_MACHINE_ID, "application.process.machine_id" },
	{ PW_KEY_APP_PROCESS_SESSION_ID, "application.process.session_id" },
	{ PW_KEY_MEDIA_ROLE, "media.role", media_role_map },
	{ "pipe.filename", "device.string" },
	{ NULL, NULL },
};
