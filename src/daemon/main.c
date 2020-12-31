/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>

#include "config.h"

#define NAME "daemon"

#define DEFAULT_CONFIG_FILE "pipewire.conf"

struct data {
	struct pw_context *context;
	struct pw_main_loop *loop;

	const char *daemon_name;
	struct pw_properties *conf;
};

static int load_config(struct data *d)
{
	const char *path;
	char filename[PATH_MAX], *data;
	struct stat sbuf;
	int fd;

	path = getenv("PIPEWIRE_CONFIG_FILE");
	if (path == NULL) {
		const char *dir;
		dir = getenv("PIPEWIRE_CONFIG_DIR");
		if (dir == NULL)
			dir = PIPEWIRE_CONFIG_DIR;
		if (dir == NULL)
			return -ENOENT;

		snprintf(filename, sizeof(filename), "%s/%s",
				dir, DEFAULT_CONFIG_FILE);
		path = filename;
	}

	if ((fd = open(path,  O_CLOEXEC | O_RDONLY)) < 0)  {
		pw_log_warn(NAME" %p: error loading config '%s': %m", d, path);
		return -errno;
	}

	pw_log_info(NAME" %p: loading config '%s'", d, path);
	if (fstat(fd, &sbuf) < 0)
		goto error_close;
	if ((data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
		goto error_close;
	close(fd);

	pw_properties_update_string(d->conf, data, sbuf.st_size);
	munmap(data, sbuf.st_size);

	return 0;

error_close:
	close(fd);
	return -errno;
}

static int parse_spa_libs(struct data *d, const char *str)
{
	struct spa_json it[2];
	char key[512], value[512];

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val;
		if (key[0] == '#') {
			if (spa_json_next(&it[1], &val) <= 0)
				break;
		}
		else if (spa_json_get_string(&it[1], value, sizeof(value)-1) > 0) {
			pw_context_add_spa_lib(d->context, key, value);
		}
	}
	return 0;
}

static int load_module(struct data *d, const char *key, const char *args, const char *flags)
{
	if (pw_context_load_module(d->context, key, args, NULL) == NULL) {
		if (errno == ENOENT && flags && strstr(flags, "ifexists") != NULL) {
			pw_log_debug(NAME" %p: skipping unavailable module %s",
					d, key);
		} else {
			pw_log_error(NAME" %p: could not load module \"%s\": %m",
					d, key);
			return -errno;
		}
	}
	return 0;
}

static int parse_modules(struct data *d, const char *str)
{
	struct spa_json it[3];
	char key[512];
	int res = 0;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val;
		char *args = NULL, *flags = NULL;
		int len;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			break;

		if (key[0] == '#')
			continue;

		if (spa_json_is_object(val, len)) {
			char arg[512], aval[1024];

			spa_json_enter(&it[1], &it[2]);

			while (spa_json_get_string(&it[2], arg, sizeof(arg)-1) > 0) {
				if (spa_json_get_string(&it[2], aval, sizeof(aval)-1) <= 0)
					break;

				if (strcmp(arg, "args") == 0) {
					args = strdup(aval);
				} else if (strcmp(arg, "flags") == 0) {
					flags = strdup(aval);
				}
			}
		}
		else if (!spa_json_is_null(val, len))
			break;

		res = load_module(d, key, args, flags);

		free(args);
		free(flags);

		if (res < 0)
			break;
	}
	return res;
}

static int create_object(struct data *d, const char *key, const char *args, const char *flags)
{
	struct pw_impl_factory *factory;
	void *obj;

	pw_log_debug("find factory %s", key);
	factory = pw_context_find_factory(d->context, key);
	if (factory == NULL) {
		if (flags && strstr(flags, "nofail") != NULL)
			return 0;
		pw_log_error("can't find factory %s", key);
		return -ENOENT;
	}
	pw_log_debug("create object with args %s", args);
	obj = pw_impl_factory_create_object(factory,
			NULL, NULL, 0,
			args ? pw_properties_new_string(args) : NULL,
			SPA_ID_INVALID);
	if (obj == NULL) {
		if (flags && strstr(flags, "nofail") != NULL)
			return 0;
		pw_log_error("can't create object from factory %s: %m", key);
		return -errno;
	}
	return 0;
}

static int parse_objects(struct data *d, const char *str)
{
	struct spa_json it[3];
	char key[512];
	int res = 0;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val;
		char *args = NULL, *flags = NULL;
		int len;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			break;

		if (key[0] == '#')
			continue;

		if (spa_json_is_object(val, len)) {
			char arg[512], aval[1024];

			spa_json_enter(&it[1], &it[2]);

			while (spa_json_get_string(&it[2], arg, sizeof(arg)-1) > 0) {
				if (spa_json_get_string(&it[2], aval, sizeof(aval)-1) <= 0)
					break;

				if (strcmp(arg, "args") == 0) {
					args = strdup(aval);
				} else if (strcmp(arg, "flags") == 0) {
					flags = strdup(aval);
				}
			}
		}
		else if (!spa_json_is_null(val, len))
			break;

		res = create_object(d, key, args, flags);

		free(args);
		free(flags);

		if (res < 0)
			break;
	}
	return res;
}

static int do_exec(struct data *d, const char *key, const char *args)
{
	int pid, res, n_args;

	pid = fork();

	if (pid == 0) {
		char *cmd, **argv;

		cmd = spa_aprintf("%s %s", key, args);
		argv = pw_split_strv(cmd, " \t", INT_MAX, &n_args);
		free(cmd);

		pw_log_info("exec %s '%s'", key, args);
		res = execvp(key, argv);
		pw_free_strv(argv);

		if (res == -1) {
			res = -errno;
			pw_log_error("execvp error '%s': %m", key);
			return res;
		}
	}
	else {
		int status;
		res = waitpid(pid, &status, WNOHANG);
		pw_log_info("exec got pid %d res:%d status:%d", pid, res, status);
	}
	return 0;
}

static int parse_exec(struct data *d, const char *str)
{
	struct spa_json it[3];
	char key[512];
	int res = 0;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val;
		char *args = NULL;
		int len;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			break;

		if (key[0] == '#')
			continue;

		if (spa_json_is_object(val, len)) {
			char arg[512], aval[1024];

			spa_json_enter(&it[1], &it[2]);

			while (spa_json_get_string(&it[2], arg, sizeof(arg)-1) > 0) {
				if (spa_json_get_string(&it[2], aval, sizeof(aval)-1) <= 0)
					break;

				if (strcmp(arg, "args") == 0)
					args = strdup(aval);
			}
		}
		else if (!spa_json_is_null(val, len))
			break;

		res = do_exec(d, key, args);

		free(args);

		if (res < 0)
			break;
	}
	return res;
}

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(struct data *d, const char *name)
{
	fprintf(stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -n, --name                            Daemon name (Default %s)\n",
		name,
		d->daemon_name);
}

int main(int argc, char *argv[])
{
	struct data d;
	struct pw_properties *properties;
	const char *str;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "name",	required_argument,	NULL, 'n' },

		{ NULL, 0, NULL, 0}
	};
	int c, res;

	if (setenv("PIPEWIRE_INTERNAL", "1", 1) < 0)
		fprintf(stderr, "can't set PIPEWIRE_INTERNAL env: %m");

	spa_zero(d);
	pw_init(&argc, &argv);

	if ((d.conf = pw_properties_new(NULL, NULL)) == NULL) {
		pw_log_error("failed to create config: %m");
		return -1;
	}

	if ((res = load_config(&d)) < 0) {
		pw_log_error("failed to load config: %s", spa_strerror(res));
		return -1;
	}

	d.daemon_name = getenv("PIPEWIRE_CORE");
	if (d.daemon_name == NULL)
		d.daemon_name = PW_DEFAULT_REMOTE;

	while ((c = getopt_long(argc, argv, "hVn:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h' :
			show_help(&d, argv[0]);
			return 0;
		case 'V' :
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'n' :
			d.daemon_name = optarg;
			fprintf(stdout, "set name %s\n", d.daemon_name);
			break;
		default:
			return -1;
		}
	}

	properties = pw_properties_new(
                                PW_KEY_CORE_NAME, d.daemon_name,
                                PW_KEY_CONTEXT_PROFILE_MODULES, "none",
                                PW_KEY_CORE_DAEMON, "true", NULL);

	/* parse configuration */
	if ((str = pw_properties_get(d.conf, "properties")) != NULL)
		pw_properties_update_string(properties, str, strlen(str));

	d.loop = pw_main_loop_new(&properties->dict);
	if (d.loop == NULL) {
		pw_log_error("failed to create main-loop: %m");
		return -1;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGINT, do_quit, &d);
	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGTERM, do_quit, &d);

	d.context = pw_context_new(pw_main_loop_get_loop(d.loop), properties, 0);
	if (d.context == NULL) {
		pw_log_error("failed to create context: %m");
		return -1;
	}

	if ((str = pw_properties_get(d.conf, "spa-libs")) != NULL)
		parse_spa_libs(&d, str);
	if ((str = pw_properties_get(d.conf, "modules")) != NULL) {
		if ((res = parse_modules(&d, str)) < 0) {
			pw_log_error("failed to load modules: %s", spa_strerror(res));
			return -1;
		}
	}
	if ((str = pw_properties_get(d.conf, "objects")) != NULL) {
		if ((res = parse_objects(&d, str)) < 0) {
			pw_log_error("failed to load objects: %s", spa_strerror(res));
			return -1;
		}
	}
	if ((str = pw_properties_get(d.conf, "exec")) != NULL) {
		if ((res = parse_exec(&d, str)) < 0) {
			pw_log_error("failed to exec: %s", spa_strerror(res));
			return -1;
		}
	}

	pw_log_info("start main loop");
	pw_main_loop_run(d.loop);
	pw_log_info("leave main loop");

	pw_properties_free(d.conf);
	pw_context_destroy(d.context);
	pw_main_loop_destroy(d.loop);
	pw_deinit();

	return 0;
}
