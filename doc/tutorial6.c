/*
  [title]
  \ref page_tutorial6
  [title]
 */
/* [code] */
#include <pipewire/pipewire.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_client *client;
	struct spa_hook client_listener;
};

/* [client_info] */
static void client_info(void *object, const struct pw_client_info *info)
{
	struct data *data = object;
	const struct spa_dict_item *item;

	printf("client: id:%u\n", info->id);
	printf("\tprops:\n");
	spa_dict_for_each(item, info->props)
		printf("\t\t%s: \"%s\"\n", item->key, item->value);

	pw_main_loop_quit(data->loop);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_info,
};
/* [client_info] */

/* [registry_event_global] */
static void registry_event_global(void *_data, uint32_t id,
			uint32_t permissions, const char *type,
			uint32_t version, const struct spa_dict *props)
{
	struct data *data = _data;
	if (data->client != NULL)
		return;

	if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
		data->client = pw_registry_bind(data->registry,
				id, type, PW_VERSION_CLIENT, 0);
		pw_client_add_listener(data->client,
				&data->client_listener,
				&client_events, data);
	}
}
/* [registry_event_global] */

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

int main(int argc, char *argv[])
{
	struct data data;

	spa_zero(data);

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL /* properties */ );
	data.context = pw_context_new(pw_main_loop_get_loop(data.loop),
				 NULL /* properties */ ,
				 0 /* user_data size */ );

	data.core = pw_context_connect(data.context, NULL /* properties */ ,
				  0 /* user_data size */ );

	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY,
					0 /* user_data size */ );

	pw_registry_add_listener(data.registry, &data.registry_listener,
				 &registry_events, &data);

	pw_main_loop_run(data.loop);

	pw_proxy_destroy((struct pw_proxy *)data.client);
	pw_proxy_destroy((struct pw_proxy *)data.registry);
	pw_core_disconnect(data.core);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
/* [code] */
