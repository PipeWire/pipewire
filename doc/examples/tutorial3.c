/*
  [title]
  \ref page_tutorial3
  [title]
 */
/* [code] */
#include <pipewire/pipewire.h>

/* [roundtrip] */
struct roundtrip_data {
	int pending;
	struct pw_main_loop *loop;
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct roundtrip_data *d = data;

	if (id == PW_ID_CORE && seq == d->pending)
		pw_main_loop_quit(d->loop);
}

static void roundtrip(struct pw_core *core, struct pw_main_loop *loop)
{
	static const struct pw_core_events core_events = {
		PW_VERSION_CORE_EVENTS,
		.done = on_core_done,
	};

	struct roundtrip_data d = { .loop = loop };
	struct spa_hook core_listener;

	pw_core_add_listener(core, &core_listener, &core_events, &d);

	d.pending = pw_core_sync(core, PW_ID_CORE, 0);

	pw_main_loop_run(loop);

	spa_hook_remove(&core_listener);
}
/* [roundtrip] */

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	printf("object: id:%u type:%s/%d\n", id, type, version);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

int main(int argc, char *argv[])
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	pw_init(&argc, &argv);

	loop = pw_main_loop_new(NULL /* properties */);
	context = pw_context_new(pw_main_loop_get_loop(loop),
			NULL /* properties */,
			0 /* user_data size */);

	core = pw_context_connect(context,
			NULL /* properties */,
			0 /* user_data size */);

	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY,
			0 /* user_data size */);

	pw_registry_add_listener(registry, &registry_listener,
				       &registry_events, NULL);

	roundtrip(core, loop);

	pw_proxy_destroy((struct pw_proxy*)registry);
	pw_core_disconnect(core);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);

	return 0;
}
/* [code] */
