
#include <glib.h>

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>

typedef struct _PipeWireSource
{
  GSource base;

  struct pw_loop *loop;
} PipeWireSource;

static gboolean
pipewire_loop_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
	PipeWireSource *s = (PipeWireSource *) source;
	int result;

	result = pw_loop_iterate (s->loop, 0);
	if (result < 0)
		g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

	return TRUE;
}

static GSourceFuncs pipewire_source_funcs =
{
	.dispatch = pipewire_loop_source_dispatch,
};

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
	GMainLoop *main_loop;
	PipeWireSource *source;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	main_loop = g_main_loop_new (NULL, FALSE);

	pw_init(&argc, &argv);

	loop = pw_loop_new(NULL /* properties */);
	/* wrap */
	source = (PipeWireSource *) g_source_new (&pipewire_source_funcs,
                                        sizeof (PipeWireSource));
	source->loop = loop;
	g_source_add_unix_fd (&source->base,
                        pw_loop_get_fd (loop),
                        G_IO_IN | G_IO_ERR);
	g_source_attach (&source->base, NULL);
	g_source_unref (&source->base);

	context = pw_context_new(loop,
			NULL /* properties */,
			0 /* user_data size */);

	core = pw_context_connect(context,
			NULL /* properties */,
			0 /* user_data size */);

	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY,
			0 /* user_data size */);

	spa_zero(registry_listener);
	pw_registry_add_listener(registry, &registry_listener,
				       &registry_events, NULL);

	/* enter and leave must be called from the same thread that runs
	 * the mainloop */
	pw_loop_enter(loop);
	g_main_loop_run(main_loop);
	pw_loop_leave(loop);

	pw_proxy_destroy((struct pw_proxy*)registry);
	pw_core_disconnect(core);
	pw_context_destroy(context);
	pw_loop_destroy(loop);

	g_main_loop_unref(main_loop);

	return 0;
}
/* [code] */
