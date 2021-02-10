/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#define NAME "config"

SPA_EXPORT
int pw_conf_load(const char *prefix, const char *name, struct pw_properties *conf)
{
	const char *path;
	char filename[PATH_MAX], *data;
	struct stat sbuf;
	int fd;
	const char *dir;
	dir = getenv("PIPEWIRE_CONFIG_DIR");
	if (dir == NULL)
		dir = PIPEWIRE_CONFIG_DIR;
	if (dir == NULL)
		return -ENOENT;

	if (prefix)
		snprintf(filename, sizeof(filename), "%s/%s/%s",
				dir, prefix, name);
	else
		snprintf(filename, sizeof(filename), "%s/%s",
				dir, name);
	path = filename;

	if ((fd = open(path,  O_CLOEXEC | O_RDONLY)) < 0)  {
		pw_log_warn(NAME" %p: error loading config '%s': %m", conf, path);
		return -errno;
	}

	pw_log_info(NAME" %p: loading config '%s'", conf, path);
	if (fstat(fd, &sbuf) < 0)
		goto error_close;
	if ((data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
		goto error_close;
	close(fd);

	pw_properties_update_string(conf, data, sbuf.st_size);
	munmap(data, sbuf.st_size);

	return 0;

error_close:
	close(fd);
	return -errno;
}

static int parse_spa_libs(struct pw_context *context, const char *str)
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
			pw_context_add_spa_lib(context, key, value);
		}
	}
	return 0;
}

static int load_module(struct pw_context *context, const char *key, const char *args, const char *flags)
{
	if (pw_context_load_module(context, key, args, NULL) == NULL) {
		if (errno == ENOENT && flags && strstr(flags, "ifexists") != NULL) {
			pw_log_debug(NAME" %p: skipping unavailable module %s",
					context, key);
		} else if (flags == NULL || strstr(flags, "nofail") == NULL) {
			pw_log_error(NAME" %p: could not load mandatory module \"%s\": %m",
					context, key);
			return -errno;
		} else {
			pw_log_info(NAME" %p: could not load optional module \"%s\": %m",
					context, key);
		}
	}
	return 0;
}

static int parse_modules(struct pw_context *context, const char *str)
{
	struct spa_json it[3];
	char key[512];
	int res = 0;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val, *aval;
		char *args = NULL, *flags = NULL;
		int len, alen;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			break;

		if (key[0] == '#')
			continue;

		if (spa_json_is_object(val, len)) {
			char arg[512];

			spa_json_enter(&it[1], &it[2]);

			while (spa_json_get_string(&it[2], arg, sizeof(arg)-1) > 0) {
				if ((alen = spa_json_next(&it[2], &aval)) <= 0)
					break;

				if (strcmp(arg, "args") == 0) {
					if (spa_json_is_container(aval, alen))
						alen = spa_json_container_len(&it[2], aval, alen);

					args = malloc(alen + 1);
					spa_json_parse_string(aval, alen, args);
				} else if (strcmp(arg, "flags") == 0) {
					if (spa_json_is_container(aval, alen))
						alen = spa_json_container_len(&it[2], aval, alen);

					flags = malloc(alen + 1);
					spa_json_parse_string(aval, alen, flags);
				}
			}
		}
		else if (!spa_json_is_null(val, len))
			break;

		res = load_module(context, key, args, flags);

		free(args);
		free(flags);

		if (res < 0)
			break;
	}
	return res;
}

static int create_object(struct pw_context *context, const char *key, const char *args, const char *flags)
{
	struct pw_impl_factory *factory;
	void *obj;

	pw_log_debug("find factory %s", key);
	factory = pw_context_find_factory(context, key);
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

static int parse_objects(struct pw_context *context, const char *str)
{
	struct spa_json it[3];
	char key[512];
	int res = 0;

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)-1) > 0) {
		const char *val, *aval;
		char *args = NULL, *flags = NULL;
		int len, alen;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			break;

		if (key[0] == '#')
			continue;

		if (spa_json_is_object(val, len)) {
			char arg[512];

			spa_json_enter(&it[1], &it[2]);

			while (spa_json_get_string(&it[2], arg, sizeof(arg)-1) > 0) {
				if ((alen = spa_json_next(&it[2], &aval)) <= 0)
					break;

				if (strcmp(arg, "args") == 0) {
					if (spa_json_is_container(aval, alen))
						alen = spa_json_container_len(&it[2], aval, alen);

					args = malloc(alen + 1);
					spa_json_parse_string(aval, alen, args);
				} else if (strcmp(arg, "flags") == 0) {
					flags = strndup(aval, alen);
				}
			}
		}
		else if (!spa_json_is_null(val, len))
			break;

		res = create_object(context, key, args, flags);

		free(args);
		free(flags);

		if (res < 0)
			break;
	}
	return res;
}

static int do_exec(struct pw_context *context, const char *key, const char *args)
{
	int pid, res, n_args;

	pid = fork();

	if (pid == 0) {
		char *cmd, **argv;

		cmd = spa_aprintf("%s %s", key, args ? args : "");
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

static int parse_exec(struct pw_context *context, const char *str)
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

		res = do_exec(context, key, args);

		free(args);

		if (res < 0)
			break;
	}
	return res;
}

SPA_EXPORT
int pw_context_parse_conf_section(struct pw_context *context,
		struct pw_properties *conf, const char *section)
{
	const char *str;
	int res;

	if ((str = pw_properties_get(conf, section)) == NULL)
		return -ENOENT;

	if (strcmp(section, "spa-libs") == 0)
		res = parse_spa_libs(context, str);
	else if (strcmp(section, "modules") == 0)
		res = parse_modules(context, str);
	else if (strcmp(section, "objects") == 0)
		res = parse_objects(context, str);
	else if (strcmp(section, "exec") == 0)
		res = parse_exec(context, str);
	else
		res = -EINVAL;

	return res;
}
