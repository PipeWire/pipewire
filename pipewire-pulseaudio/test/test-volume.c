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

#include <stdio.h>
#include <errno.h>

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/timeval.h>
#include <pulse/subscribe.h>
#include <pulse/volume.h>

struct data {
	pa_mainloop *loop;
	pa_context *context;
	pa_time_event *timer;

	int n_channels;
	int cycle;
};

static void time_event_cb(pa_mainloop_api*a, pa_time_event* e, const struct timeval *tv, void *userdata)
{
	struct data *data = userdata;
	pa_cvolume volume;
	pa_volume_t vol;

	if (data->cycle++ & 1)
		vol = PA_VOLUME_NORM / 2;
	else
		vol = PA_VOLUME_NORM / 3;

	pa_cvolume_set(&volume, data->n_channels, vol);

	fprintf(stderr, "set volume\n");
	pa_context_set_sink_volume_by_name(data->context,
			"@DEFAULT_SINK@", &volume, NULL, NULL);
}

static void start_timer(struct data *data)
{
	struct timeval tv;
	pa_mainloop_api *api = pa_mainloop_get_api(data->loop);

	pa_gettimeofday(&tv);
	pa_timeval_add(&tv, 1 * PA_USEC_PER_SEC);

	if (data->timer == NULL) {
		data->timer = api->time_new(api, &tv, time_event_cb, data);
	} else {
		api->time_restart(data->timer, &tv);
	}
}

static void context_state_callback(pa_context *c, void *userdata)
{
	struct data *data = userdata;

	fprintf(stderr, "context state: %d\n", pa_context_get_state(c));

	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
		break;
	case PA_CONTEXT_READY:
		pa_context_subscribe(data->context,
				PA_SUBSCRIPTION_MASK_SINK|
				PA_SUBSCRIPTION_MASK_SOURCE|
				PA_SUBSCRIPTION_MASK_CLIENT|
				PA_SUBSCRIPTION_MASK_SINK_INPUT|
				PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT|
				PA_SUBSCRIPTION_MASK_CARD|
				PA_SUBSCRIPTION_MASK_MODULE|
				PA_SUBSCRIPTION_MASK_SERVER,
				NULL, NULL);
		start_timer(data);
		break;
	case PA_CONTEXT_TERMINATED:
	case PA_CONTEXT_FAILED:
	default:
		pa_mainloop_quit(data->loop, -1);
		break;
	}
}

static const char *str_etype(pa_subscription_event_type_t event)
{
	switch (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
	case PA_SUBSCRIPTION_EVENT_NEW:
		return "new";
	case PA_SUBSCRIPTION_EVENT_CHANGE:
		return "change";
	case PA_SUBSCRIPTION_EVENT_REMOVE:
		return "remove";
	}
	return "invalid";
}

static const char *str_efac(pa_subscription_event_type_t event)
{
	switch (event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
	case PA_SUBSCRIPTION_EVENT_SINK:
		return "sink";
	case PA_SUBSCRIPTION_EVENT_SOURCE:
		return "source";
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
		return "sink-input";
	case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
		return "source-output";
	case PA_SUBSCRIPTION_EVENT_MODULE:
		return "module";
	case PA_SUBSCRIPTION_EVENT_CLIENT:
		return "client";
	case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE:
		return "sample-cache";
	case PA_SUBSCRIPTION_EVENT_SERVER:
		return "server";
	case PA_SUBSCRIPTION_EVENT_AUTOLOAD:
		return "autoload";
	case PA_SUBSCRIPTION_EVENT_CARD:
		return "card";
	}
	return "invalid";
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
	struct data *data = userdata;
	char v[1024];

	if (eol < 0) {
		fprintf(stderr, "sink info: error:%s", pa_strerror(pa_context_errno(c)));
		return;
	}
	if (eol)
		return;

	fprintf(stderr, "sink info: index:%d\n", i->index);
	fprintf(stderr, "\tname:%s\n", i->name);
	fprintf(stderr, "\tdescription:%s\n", i->description);
	fprintf(stderr, "\tmute:%s\n", i->mute ? "yes" : "no");
	fprintf(stderr, "\tvolume:%s\n", pa_cvolume_snprint_verbose(v, sizeof(v),
				&i->volume, &i->channel_map, i->flags & PA_SINK_DECIBEL_VOLUME));
	fprintf(stderr, "\tbalance:%0.2f\n", pa_cvolume_get_balance(&i->volume, &i->channel_map));
	fprintf(stderr, "\tbase:%s\n", pa_volume_snprint_verbose(v, sizeof(v),
				i->base_volume, i->flags & PA_SINK_DECIBEL_VOLUME));

	data->n_channels = i->volume.channels;

	start_timer(data);
}

static void context_subscribe_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
	struct data *data = userdata;

	fprintf(stderr, "subscribe event %d (%s|%s), idx:%d\n", t,
			str_etype(t), str_efac(t), idx);

	switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
	case PA_SUBSCRIPTION_EVENT_SINK:
		pa_context_get_sink_info_by_name(data->context,
				"@DEFAULT_SINK@", sink_info_cb, data);
		break;
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	pa_mainloop_api *api;
	int ret;

	data.loop = pa_mainloop_new();
	data.n_channels = 1;

	api = pa_mainloop_get_api(data.loop);
	data.context = pa_context_new(api, "test-volume");

	pa_context_set_state_callback(data.context, context_state_callback, &data);

	if (pa_context_connect(data.context, NULL, 0, NULL) < 0) {
		fprintf(stderr, "pa_context_connect() failed.\n");
		return -1;
	}
	pa_context_set_subscribe_callback(data.context, context_subscribe_cb, &data);

	pa_mainloop_run(data.loop, &ret);

	return 0;
}
