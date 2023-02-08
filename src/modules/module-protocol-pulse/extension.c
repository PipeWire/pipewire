/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/defs.h>
#include <spa/utils/string.h>

#include "defs.h"
#include "extension.h"
#include "extensions/registry.h"

static const struct extension extensions[] = {
	{ "module-stream-restore", 0 | MODULE_EXTENSION_FLAG, do_extension_stream_restore, },
	{ "module-device-restore", 1 | MODULE_EXTENSION_FLAG, do_extension_device_restore, },
	{ "module-device-manager", 2 | MODULE_EXTENSION_FLAG, do_extension_device_manager, },
};

const struct extension *extension_find(uint32_t index, const char *name)
{
	SPA_FOR_EACH_ELEMENT_VAR(extensions, ext) {
		if (index == ext->index || spa_streq(name, ext->name))
			return ext;
	}
	return NULL;
}
