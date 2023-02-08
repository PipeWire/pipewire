/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/list.h>
#include <spa/utils/hook.h>
#include <pipewire/work-queue.h>

#include "client.h"
#include "internal.h"
#include "log.h"
#include "operation.h"
#include "pending-sample.h"
#include "sample-play.h"

void pending_sample_free(struct pending_sample *ps)
{
	struct client * const client = ps->client;
	struct impl * const impl = client->impl;
	struct operation *o;

	spa_list_remove(&ps->link);
	spa_hook_remove(&ps->listener);
	pw_work_queue_cancel(impl->work_queue, ps, SPA_ID_INVALID);

	if ((o = operation_find(client, ps->tag)) != NULL)
		operation_free(o);

	sample_play_destroy(ps->play);
}
