#include "config.h"

#include <limits.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <spa/utils/json.h>
#include <spa/support/log.h>
#include <spa/utils/cleanup.h>

#include "audio-plugin.h"

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_log *log;
};

/* pipe */
struct pipe_impl {
	struct plugin *plugin;

	struct spa_log *log;
	unsigned long rate;
	float *port[3];
	float latency;

	int write_fd;
	int read_fd;
	size_t written;
	size_t read;
};

static int do_exec(struct pipe_impl *impl, const char *command)
{
	int pid, res, len, argc = 0;
	char *argv[512];
	struct spa_json it[2];
	const char *value;
	int stdin_pipe[2];
	int stdout_pipe[2];

        if (spa_json_begin_array_relax(&it[0], command, strlen(command)) <= 0)
                return -EINVAL;

        while ((len = spa_json_next(&it[0], &value)) > 0) {
                char *s;

                if (argc >= (int)SPA_N_ELEMENTS(argv) - 1) {
                        spa_log_error(impl->log, "too many exec arguments");
                        res = -E2BIG;
                        goto error_argv;
                }
                if ((s = malloc(len+1)) == NULL) {
                        res = -errno;
                        goto error_argv;
                }

                spa_json_parse_stringn(value, len, s, len+1);

		argv[argc++] = s;
        }
	argv[argc++] = NULL;

	if (pipe2(stdin_pipe, 0) < 0) {
		res = -errno;
		spa_log_error(impl->log, "pipe2 error: %m");
		goto error_argv;
	}
	if (pipe2(stdout_pipe, 0) < 0) {
		res = -errno;
		spa_log_error(impl->log, "pipe2 error: %m");
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		goto error_argv;
	}

	impl->write_fd = stdin_pipe[1];
	impl->read_fd = stdout_pipe[0];

	pid = fork();

	if (pid == 0) {
		char buf[1024];
		char *const *p;
		struct spa_strbuf s;

		/* Double fork to avoid zombies; we don't want to set SIGCHLD handler */
		pid = fork();

		if (pid < 0) {
			spa_log_error(impl->log, "fork error: %m");
			goto done;
		} else if (pid != 0) {
			exit(0);
		}

		dup2(stdin_pipe[0], 0);
		dup2(stdout_pipe[1], 1);

		spa_strbuf_init(&s, buf, sizeof(buf));
		for (p = argv; *p; ++p)
			spa_strbuf_append(&s, " '%s'", *p);

		spa_log_info(impl->log, "exec%s", s.buffer);
		res = execvp(argv[0], argv);

		if (res == -1) {
			res = -errno;
			spa_log_error(impl->log, "execvp error '%s': %m", argv[0]);
		}
done:
		exit(1);
	} else if (pid < 0) {
		spa_log_error(impl->log, "fork error: %m");
	} else {
		int status = 0;
		close(stdin_pipe[0]);
		close(stdout_pipe[1]);
		do {
			errno = 0;
			res = waitpid(pid, &status, 0);
		} while (res < 0 && errno == EINTR);
		spa_log_debug(impl->log, "exec got pid %d res:%d status:%d", (int)pid, res, status);
	}
	res = 0;
error_argv:
	while (argc > 0)
		free(argv[--argc]);
	return res;
}

static void pipe_transfer(struct pipe_impl *impl, float *in, float *out, int count)
{
	ssize_t sz;

	sz = read(impl->read_fd, out, count * sizeof(float));
	if (sz > 0) {
		impl->read += sz;
		if (impl->read == (size_t)sz) {
			while ((sz = read(impl->read_fd, out, count * sizeof(float))) != -1)
				impl->read += sz;
		}
	} else {
		memset(out, 0, count * sizeof(float));
	}
	if ((sz = write(impl->write_fd, in, count * sizeof(float))) != -1)
		impl->written += sz;
}

static void *pipe_instantiate(const struct spa_fga_plugin *plugin, const struct spa_fga_descriptor * Descriptor,
		unsigned long SampleRate, int index, const char *config)
{
	struct plugin *pl = SPA_CONTAINER_OF(plugin, struct plugin, plugin);
	struct pipe_impl *impl;
	struct spa_json it[2];
	const char *val;
	char key[256];
	spa_autofree char*command = NULL;
	int len;

	errno = EINVAL;
	if (config == NULL) {
		spa_log_error(pl->log, "pipe: requires a config section");
		return NULL;
	}

	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(pl->log, "pipe: config must be an object");
		return NULL;
	}

	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "command")) {
			if ((command = malloc(len+1)) == NULL)
				return NULL;

			if (spa_json_parse_stringn(val, len, command, len+1) <= 0) {
				spa_log_error(pl->log, "pipe: command requires a string");
				return NULL;
			}
		}
		else {
			spa_log_warn(pl->log, "pipe: ignoring config key: '%s'", key);
		}
	}
	if (command == NULL || command[0] == '\0') {
		spa_log_error(pl->log, "pipe: command must be given and can not be empty");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->plugin = pl;
	impl->log = pl->log;
	impl->rate = SampleRate;

	do_exec(impl, command);

	int flags;
	if ((flags = fcntl(impl->write_fd, F_GETFL)) >= 0)
		fcntl(impl->write_fd, F_SETFL, flags | O_NONBLOCK);
	if ((flags = fcntl(impl->read_fd, F_GETFL)) >= 0)
		fcntl(impl->read_fd, F_SETFL, flags | O_NONBLOCK);

	return impl;
}

static void pipe_connect_port(void *Instance, unsigned long Port, void * DataLocation)
{
	struct pipe_impl *impl = Instance;
	impl->port[Port] = DataLocation;
}

static void pipe_run(void * Instance, unsigned long SampleCount)
{
	struct pipe_impl *impl = Instance;
	float *in = impl->port[0], *out = impl->port[1];

	if (in != NULL && out != NULL)
		pipe_transfer(impl, in, out, SampleCount);
}

static void pipe_cleanup(void * Instance)
{
	struct pipe_impl *impl = Instance;
	close(impl->write_fd);
	close(impl->read_fd);
	free(impl);
}

static struct spa_fga_port pipe_ports[] = {
	{ .index = 0,
	  .name = "In",
	  .flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO,
	},
	{ .index = 1,
	  .name = "Out",
	  .flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO,
	},
};

static const struct spa_fga_descriptor pipe_desc = {
	.name = "pipe",
	.flags = SPA_FGA_DESCRIPTOR_SUPPORTS_NULL_DATA,

	.n_ports = SPA_N_ELEMENTS(pipe_ports),
	.ports = pipe_ports,

	.instantiate = pipe_instantiate,
	.connect_port = pipe_connect_port,
	.run = pipe_run,
	.cleanup = pipe_cleanup,
};

static const struct spa_fga_descriptor * pipe_descriptor(unsigned long Index)
{
	switch(Index) {
	case 0:
		return &pipe_desc;
	}
	return NULL;
}


static const struct spa_fga_descriptor *pipe_plugin_make_desc(void *plugin, const char *name)
{
	unsigned long i;
	for (i = 0; ;i++) {
		const struct spa_fga_descriptor *d = pipe_descriptor(i);
		if (d == NULL)
			break;
		if (spa_streq(d->name, name))
			return d;
	}
	return NULL;
}

static struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = pipe_plugin_make_desc,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct plugin *impl;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	impl = (struct plugin *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin))
		*interface = &impl->plugin;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct plugin);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct plugin *impl;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	impl = (struct plugin *) handle;

	impl->plugin.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN,
			&impl_plugin, impl);

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static struct spa_handle_factory spa_fga_pipe_plugin_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.pipe",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_fga_pipe_plugin_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
