/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/defs.h>
#include <spa/utils/string.h>

#include "client.h"
#include "defs.h"
#include "extension.h"
#include "message.h"
#include "module.h"

static const struct extension *find_extension_command(struct module *module, uint32_t command)
{
	uint32_t i;

	if (module->info->extension == NULL)
		return NULL;

	for (i = 0; module->info->extension[i].name; i++) {
		if (module->info->extension[i].command == command)
			return &module->info->extension[i];
	}
	return NULL;
}

int extension_process(struct module *module, struct client *client, uint32_t tag, struct message *m)
{
	uint32_t command;
	const struct extension *ext;
	int res;

	if ((res = message_get(m,
			TAG_U32, &command,
			TAG_INVALID)) < 0)
		return -EPROTO;

	ext = find_extension_command(module, command);
	if (ext == NULL)
		return -ENOTSUP;
	if (ext->process == NULL)
		return -EPROTO;

	pw_log_info("client %p [%s]: %s %s tag:%u",
		    client, client->name, module->info->name, ext->name, tag);

	return ext->process(module, client, command, tag, m);
}
