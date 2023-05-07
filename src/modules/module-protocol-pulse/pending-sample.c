/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/list.h>
#include <spa/utils/hook.h>
#include <pipewire/work-queue.h>

#include "client.h"
#include "collect.h"
#include "commands.h"
#include "internal.h"
#include "log.h"
#include "message.h"
#include "operation.h"
#include "pending-sample.h"
#include "reply.h"
#include "sample-play.h"

static void do_pending_sample_finish(void *obj, void *data, int res, uint32_t id)
{
	struct pending_sample *ps = obj;
	struct client *client = ps->client;

	pending_sample_free(ps);
	client_unref(client);
}

static void schedule_maybe_finish(struct pending_sample *ps)
{
	if (!ps->done || !ps->replied)
		return;

	pw_work_queue_add(ps->client->impl->work_queue, ps, 0,
			  do_pending_sample_finish, NULL);
}

static void sample_play_ready_reply(void *data, struct client *client, uint32_t tag)
{
	struct pending_sample *ps = data;
	uint32_t index = id_to_index(client->manager, ps->play->id);

	pw_log_info("[%s] PLAY_SAMPLE tag:%u index:%u",
			client->name, ps->tag, index);

	if (!ps->replied) {
		struct message *reply = reply_new(client, ps->tag);
		if (client->version >= 13)
			message_put(reply,
				TAG_U32, index,
				TAG_INVALID);

		client_queue_message(client, reply);
		ps->replied = true;
	}

	schedule_maybe_finish(ps);
}

static void on_sample_play_ready(void *data, uint32_t id)
{
	struct pending_sample *ps = data;
	struct client *client = ps->client;

	if (!ps->replied)
		operation_new_cb(client, ps->tag, sample_play_ready_reply, ps);
}

static void on_sample_play_done(void *data, int res)
{
	struct pending_sample *ps = data;
	struct client *client = ps->client;

	if (!ps->replied && res < 0) {
		reply_error(client, COMMAND_PLAY_SAMPLE, ps->tag, res);
		ps->replied = true;
	}

	pw_log_info("[%s] PLAY_SAMPLE done tag:%u result:%d", client->name, ps->tag, res);

	ps->done = true;
	schedule_maybe_finish(ps);
}

static const struct sample_play_events sample_play_events = {
	VERSION_SAMPLE_PLAY_EVENTS,
	.ready = on_sample_play_ready,
	.done = on_sample_play_done,
};

static void on_client_disconnect(void *data)
{
	struct pending_sample *ps = data;

	ps->replied = true;
	operation_free_by_tag(ps->client, ps->tag);

	schedule_maybe_finish(ps);
}

static const struct client_events client_events = {
	VERSION_CLIENT_EVENTS,
	.disconnect = on_client_disconnect,
};

int pending_sample_new(struct client *client, struct sample *sample, struct pw_properties *props, uint32_t tag)
{
	struct pending_sample *ps;
	struct sample_play *p = sample_play_new(client->core, sample, props, sizeof(*ps));
	if (!p)
		return -errno;

	ps = p->user_data;
	ps->client = client;
	ps->play = p;
	ps->tag = tag;
	sample_play_add_listener(p, &ps->listener, &sample_play_events, ps);
	client_add_listener(client, &ps->client_listener, &client_events, ps);
	spa_list_append(&client->pending_samples, &ps->link);
	client->ref++;

	return 0;
}

void pending_sample_free(struct pending_sample *ps)
{
	struct client * const client = ps->client;
	struct impl * const impl = client->impl;

	spa_list_remove(&ps->link);
	spa_hook_remove(&ps->listener);
	spa_hook_remove(&ps->client_listener);
	pw_work_queue_cancel(impl->work_queue, ps, SPA_ID_INVALID);

	operation_free_by_tag(client, ps->tag);

	sample_play_destroy(ps->play);
}
