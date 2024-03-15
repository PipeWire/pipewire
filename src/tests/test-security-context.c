/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/extensions/security-context.h>

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
	} test = { PW_VERSION_SECURITY_CONTEXT_EVENTS,  };
	struct pw_security_context_events ev;

	spa_assert_se(PW_VERSION_SECURITY_CONTEXT_EVENTS == 0);
	spa_assert_se(sizeof(ev) == sizeof(test));
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

	while (!data.done)
		pw_main_loop_run(loop);

	spa_hook_remove(&core_listener);
	return 0;
}

struct registry_info {
	struct pw_registry *registry;
	struct pw_security_context *sec;
};

static void registry_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct registry_info *info = data;

	if (spa_streq(type, PW_TYPE_INTERFACE_SecurityContext)) {
		info->sec = pw_registry_bind(info->registry, id, type, version, 0);
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global
};

static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct registry_info info;
	struct spa_hook listener;
	int res, listen_fd, close_fd[2];
	char temp[PATH_MAX] = "/tmp/pipewire-XXXXXX";
	struct sockaddr_un sockaddr = {0};

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert_se(context != NULL);
	core = pw_context_connect(context, NULL, 0);
	if (core == NULL && errno == EHOSTDOWN)
		return;
	spa_assert_se(core != NULL);

	spa_zero(info);
	info.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	spa_assert_se(info.registry != NULL);

	pw_registry_add_listener(info.registry, &listener, &registry_events, &info);

	roundtrip(core, loop);

	spa_assert_se(info.sec != NULL);

	res = mkstemp(temp);
	spa_assert_se(res >= 0);
	close(res);

	unlink(temp);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	spa_assert_se(listen_fd >= 0);

	sockaddr.sun_family = AF_UNIX;
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", temp);
	if (bind(listen_fd, (struct sockaddr *) &sockaddr, sizeof (sockaddr)) != 0)
		spa_assert_not_reached();

	if (listen(listen_fd, 0) != 0)
		spa_assert_not_reached();

	res = pipe2(close_fd, O_CLOEXEC);
	spa_assert_se(res >= 0);

	static const struct spa_dict_item items[] = {
		{ "pipewire.foo.bar", "baz" },
		{ PW_KEY_SEC_ENGINE, "org.flatpak" },
		{ PW_KEY_ACCESS, "restricted" },
	};

	pw_security_context_create(info.sec,
			listen_fd, close_fd[1],
			&SPA_DICT_INIT_ARRAY(items));

	roundtrip(core, loop);

	unlink(temp);

	pw_proxy_destroy((struct pw_proxy*)info.sec);
	pw_proxy_destroy((struct pw_proxy*)info.registry);

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();

	pw_deinit();

	return 0;
}
