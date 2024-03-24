/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <unistd.h>
#include <limits.h>
#include <locale.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/ansi.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/security-context.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_properties *props;

	struct pw_security_context *sec;

	int pending_create;
	int create_result;
	int pending;
	int done;
};

static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;

	if (spa_streq(type, PW_TYPE_INTERFACE_SecurityContext))
		d->sec = pw_registry_bind(d->registry, id, type, version, 0);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (seq == SPA_RESULT_ASYNC_SEQ(data->pending_create))
		data->create_result = res;

	if (id == PW_ID_CORE && res == -EPIPE) {
		data->done = true;
		pw_main_loop_quit(data->loop);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void core_event_done(void *object, uint32_t id, int seq)
{
	struct data *data = object;
	if (id == PW_ID_CORE && seq == data->pending) {
		data->done = true;
		pw_main_loop_quit(data->loop);
	}
}

static int roundtrip(struct data *data)
{
	struct spa_hook core_listener;
	const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
		.done = core_event_done,
	};
	spa_zero(core_listener);
	pw_core_add_listener(data->core, &core_listener,
			&core_events, data);

	data->done = false;
	data->pending = pw_core_sync(data->core, PW_ID_CORE, 0);

	while (!data->done)
		pw_main_loop_run(data->loop);

	spa_hook_remove(&core_listener);
	return 0;
}


static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(struct data *d, const char *name, bool error)
{
	FILE *out = error ? stderr : stdout;
	fprintf(out, "%s [options] [application]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -P, --properties                      Context properties\n",
		name);
	fprintf(out, "\nDefault Context properties:\n");
	pw_properties_serialize_dict(out, &d->props->dict,
			PW_PROPERTIES_FLAG_NL | PW_PROPERTIES_FLAG_ENCLOSE);
	fprintf(out, "\n");
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const char *opt_remote = NULL;
	static const struct option long_options[] = {
		{ "help",		no_argument,		NULL, 'h' },
		{ "version",		no_argument,		NULL, 'V' },
		{ "remote",		required_argument,	NULL, 'r' },
		{ "properties",		required_argument,	NULL, 'P' },
		{ NULL,	0, NULL, 0}
	};
	int c, res, listen_fd, close_fd[2], line, col;
	char temp[PATH_MAX] = "/tmp/pipewire-XXXXXX";
	struct sockaddr_un sockaddr = {0};

	data.props = pw_properties_new(
			PW_KEY_SEC_ENGINE, "org.flatpak",
			PW_KEY_ACCESS, "restricted",
			NULL);

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVr:P:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0], false);
			return 0;
		case 'V':
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			opt_remote = optarg;
			break;
		case 'P':
			if (!pw_properties_check_string(optarg, strlen(optarg), &line, &col)) {
				fprintf(stderr, "error: syntax error in --properties at line:%d col:%d\n", line, col);
				return -1;
			}
			pw_properties_update_string(data.props, optarg, strlen(optarg));
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create main loop: %m\n");
		return -1;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		return -1;
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote ? opt_remote :
					("[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]"),
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		return -1;
	}

	pw_core_add_listener(data.core,
				   &data.core_listener,
				   &core_events, &data);
	data.registry = pw_core_get_registry(data.core,
					  PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
				       &data.registry_listener,
				       &registry_events, &data);

	roundtrip(&data);

	if (data.sec == NULL) {
		fprintf(stderr, "no security context object found\n");
		return -1;
	}

	res = mkstemp(temp);
	if (res < 0) {
		fprintf(stderr, "can't make temp file with template %s: %m\n", temp);
		return -1;
	}
	close(res);
	unlink(temp);

	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		fprintf(stderr, "can't make unix socket: %m\n");
		return -1;
	}

	sockaddr.sun_family = AF_UNIX;
	snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", temp);
	if (bind(listen_fd, (struct sockaddr *) &sockaddr, sizeof (sockaddr)) != 0) {
		fprintf(stderr, "can't bind unix socket to %s: %m\n", temp);
		return -1;
	}
	if (listen(listen_fd, 0) != 0) {
		fprintf(stderr, "can't listen unix socket: %m\n");
		return -1;
	}

	res = pipe2(close_fd, O_CLOEXEC);
	if (res < 0) {
		fprintf(stderr, "can't create pipe: %m\n");
		return -1;
	}
	setenv("PIPEWIRE_REMOTE", temp, 1);

	data.create_result = 0;
	data.pending_create = pw_security_context_create(data.sec,
			listen_fd, close_fd[1], &data.props->dict);

	if (SPA_RESULT_IS_ASYNC(data.pending_create)) {
		pw_log_debug("create: %d", data.pending_create);
		roundtrip(&data);
	}
	pw_log_debug("create result: %d", data.create_result);

	if (data.create_result < 0) {
		fprintf(stderr, "can't create security context: %s\n",
				spa_strerror(data.create_result));
		return -1;
	}

	if (optind < argc) {
		system(argv[optind++]);
	} else {
		fprintf(stdout, "new socket: %s\n", temp);
		pw_main_loop_run(data.loop);
	}
	unlink(temp);

	spa_hook_remove(&data.registry_listener);
	pw_proxy_destroy((struct pw_proxy*)data.sec);
	pw_proxy_destroy((struct pw_proxy*)data.registry);
	spa_hook_remove(&data.core_listener);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_properties_free(data.props);
	pw_deinit();

	return 0;
}
