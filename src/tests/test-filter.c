/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/filter.h>

#include <spa/utils/string.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	spa_assert_se(SPA_PTRDIFF(&a.func, &a) == SPA_PTRDIFF(&b.func, &b)); \
} while(0)

static void test_abi(void)
{
	static const struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*state_changed) (void *data, enum pw_filter_state old,
			enum pw_filter_state state, const char *error);
		void (*io_changed) (void *data, void *port_data, uint32_t id, void *area, uint32_t size);
		void (*param_changed) (void *data, void *port_data, uint32_t id, const struct spa_pod *param);
		void (*add_buffer) (void *data, void *port_data, struct pw_buffer *buffer);
		void (*remove_buffer) (void *data, void *port_data, struct pw_buffer *buffer);
		void (*process) (void *data, struct spa_io_position *position);
		void (*drained) (void *data);
		void (*command) (void *data, const struct spa_command *command);
	} test = { PW_VERSION_FILTER_EVENTS, NULL };

	struct pw_filter_events ev;

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, state_changed);
	TEST_FUNC(ev, test, io_changed);
	TEST_FUNC(ev, test, param_changed);
	TEST_FUNC(ev, test, add_buffer);
	TEST_FUNC(ev, test, remove_buffer);
	TEST_FUNC(ev, test, process);
	TEST_FUNC(ev, test, drained);
	TEST_FUNC(ev, test, command);

	spa_assert_se(PW_VERSION_FILTER_EVENTS == 1);
	spa_assert_se(sizeof(ev) == sizeof(test));

	spa_assert_se(PW_FILTER_STATE_ERROR == -1);
	spa_assert_se(PW_FILTER_STATE_UNCONNECTED == 0);
	spa_assert_se(PW_FILTER_STATE_CONNECTING == 1);
	spa_assert_se(PW_FILTER_STATE_PAUSED == 2);
	spa_assert_se(PW_FILTER_STATE_STREAMING == 3);

	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_ERROR) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_UNCONNECTED) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_CONNECTING) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_PAUSED) != NULL);
	spa_assert_se(pw_filter_state_as_string(PW_FILTER_STATE_STREAMING) != NULL);
}

static void filter_destroy_error(void *data)
{
	spa_assert_not_reached();
}
static void filter_state_changed_error(void *data, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	spa_assert_not_reached();
}
static void filter_io_changed_error(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	spa_assert_not_reached();
}
static void filter_param_changed_error(void *data, void *port_data, uint32_t id, const struct spa_pod *format)
{
	spa_assert_not_reached();
}
static void filter_add_buffer_error(void *data, void *port_data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void filter_remove_buffer_error(void *data, void *port_data, struct pw_buffer *buffer)
{
	spa_assert_not_reached();
}
static void filter_process_error(void *data, struct spa_io_position *position)
{
	spa_assert_not_reached();
}
static void filter_drained_error(void *data)
{
	spa_assert_not_reached();
}

static const struct pw_filter_events filter_events_error =
{
	PW_VERSION_FILTER_EVENTS,
        .destroy = filter_destroy_error,
        .state_changed = filter_state_changed_error,
	.io_changed = filter_io_changed_error,
	.param_changed = filter_param_changed_error,
	.add_buffer = filter_add_buffer_error,
	.remove_buffer = filter_remove_buffer_error,
	.process = filter_process_error,
	.drained = filter_drained_error
};

static int destroy_count = 0;
static void filter_destroy_count(void *data)
{
	destroy_count++;
}
static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_filter *filter;
	struct pw_filter_events filter_events = filter_events_error;
	struct spa_hook listener = { 0, };
	const char *error = NULL;

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test", NULL);
	spa_assert_se(filter != NULL);
	pw_filter_add_listener(filter, &listener, &filter_events, filter);

	/* check state */
	spa_assert_se(pw_filter_get_state(filter, &error) == PW_FILTER_STATE_UNCONNECTED);
	spa_assert_se(error == NULL);
	/* check name */
	spa_assert_se(spa_streq(pw_filter_get_name(filter), "test"));

	/* check id, only when connected */
	spa_assert_se(pw_filter_get_node_id(filter) == SPA_ID_INVALID);

	/* check destroy */
	destroy_count = 0;
	filter_events.destroy = filter_destroy_count;
	pw_filter_destroy(filter);
	spa_assert_se(destroy_count == 1);

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

static void test_properties(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	const struct pw_properties *props;
	struct pw_filter *filter;
	struct pw_filter_events filter_events = filter_events_error;
	struct spa_hook listener = { { NULL }, };
	struct spa_dict_item items[3];

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test",
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL));
	spa_assert_se(filter != NULL);
	pw_filter_add_listener(filter, &listener, &filter_events, filter);

	props = pw_filter_get_properties(filter, NULL);
	spa_assert_se(props != NULL);
	spa_assert_se(spa_streq(pw_properties_get(props, "foo"), "bar"));
	spa_assert_se(spa_streq(pw_properties_get(props, "biz"), "fuzz"));
	spa_assert_se(pw_properties_get(props, "buzz") == NULL);

	/* remove foo */
	items[0] = SPA_DICT_ITEM_INIT("foo", NULL);
	/* change biz */
	items[1] = SPA_DICT_ITEM_INIT("biz", "buzz");
	/* add buzz */
	items[2] = SPA_DICT_ITEM_INIT("buzz", "frizz");
	pw_filter_update_properties(filter, NULL, &SPA_DICT_INIT(items, 3));

	spa_assert_se(props == pw_filter_get_properties(filter, NULL));
	spa_assert_se(pw_properties_get(props, "foo") == NULL);
	spa_assert_se(spa_streq(pw_properties_get(props, "biz"), "buzz"));
	spa_assert_se(spa_streq(pw_properties_get(props, "buzz"), "frizz"));

	/* check destroy */
	destroy_count = 0;
	filter_events.destroy = filter_destroy_count;
	pw_context_destroy(context);
	spa_assert_se(destroy_count == 1);

	pw_main_loop_destroy(loop);
}

struct roundtrip_data
{
	struct pw_main_loop *loop;
	int pending;
	int done;
};

static void core_event_done(void *object, uint32_t id, int seq)
{
	struct roundtrip_data *data = object;
	if (id == PW_ID_CORE && seq == data->pending) {
		data->done = 1;
		printf("done %d\n", seq);
		pw_main_loop_quit(data->loop);
	}
}

static int roundtrip(struct pw_core *core, struct pw_main_loop *loop)
{
	struct spa_hook core_listener;
	struct roundtrip_data data = { .loop = loop };
	const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
		.done = core_event_done,
	};
	spa_zero(core_listener);
	pw_core_add_listener(core, &core_listener,
			&core_events, &data);

	data.pending = pw_core_sync(core, PW_ID_CORE, 0);
	printf("sync %d\n", data.pending);

	while (!data.done) {
		pw_main_loop_run(loop);
	}
	spa_hook_remove(&core_listener);
	return 0;
}

static int node_count = 0;
static int port_count = 0;
static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	printf("object: id:%u type:%s/%d\n", id, type, version);
	if (spa_streq(type, PW_TYPE_INTERFACE_Port))
		port_count++;
	else if (spa_streq(type, PW_TYPE_INTERFACE_Node))
		node_count++;

}
static void registry_event_global_remove(void *data, uint32_t id)
{
	printf("object: id:%u\n", id);
}

struct port {
	struct pw_filter *filter;
};

static void test_create_port(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_filter *filter;
	struct spa_hook registry_listener = { 0, };
	static const struct pw_registry_events registry_events = {
		PW_VERSION_REGISTRY_EVENTS,
		.global = registry_event_global,
		.global_remove = registry_event_global_remove,
	};
	int res;
	struct port *port;
	enum pw_filter_state state;

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect_self(context, NULL, 0);
	spa_assert_se(core != NULL);
	filter = pw_filter_new(core, "test", NULL);
	spa_assert_se(filter != NULL);

	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	spa_assert_se(registry != NULL);
        pw_registry_add_listener(registry, &registry_listener,
                                       &registry_events, NULL);

	state = pw_filter_get_state(filter, NULL);
	printf("state %s\n", pw_filter_state_as_string(state));
	res = pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0);
	spa_assert_se(res >= 0);

	printf("wait connect\n");
	while (true) {
		state = pw_filter_get_state(filter, NULL);
		printf("state %s\n", pw_filter_state_as_string(state));
		spa_assert_se(state != PW_FILTER_STATE_ERROR);

		if (state == PW_FILTER_STATE_PAUSED)
			break;

		roundtrip(core, loop);
	}
	spa_assert_se(node_count == 1);

	printf("add port\n");
	/* make an audio DSP output port */
	port = pw_filter_add_port(filter,
			PW_DIRECTION_OUTPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			sizeof(struct port),
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				PW_KEY_PORT_NAME, "output",
				NULL),
			NULL, 0);

	printf("wait port\n");
	roundtrip(core, loop);

	spa_assert_se(port_count == 1);
	printf("port added\n");

	printf("remove port\n");
	pw_filter_remove_port(port);
	roundtrip(core, loop);

	printf("destroy\n");
	/* check destroy */
	pw_filter_destroy(filter);

	pw_proxy_destroy((struct pw_proxy*)registry);

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();
	test_properties();
	test_create_port();

	pw_deinit();

	return 0;
}
