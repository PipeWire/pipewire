/* PipeWire
 *
 * Copyright © 2022 Wim Taymans
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

#include <spa/utils/json.h>

#include <pipewire/utils.h>

#include "module.h"
#include "cmd.h"

static const char WHITESPACE[] = " \t\n\r";

static int do_load_module(struct impl *impl, char *args, const char *flags)
{
	int res, n;
	struct module *module;
	char *a[2] = { NULL };

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		pw_log_info("load-module expects module name");
		return -EINVAL;
	}

	module = module_create(impl, a[0], a[1]);
	if (module == NULL)
		return -errno;
	if ((res = module_load(module)) < 0)
		return res;

	return res;
}

static int do_cmd(struct impl *impl, const char *cmd, char *args, const char *flags)
{
	int res = 0;
	if (spa_streq(cmd, "load-module")) {
		res = do_load_module(impl, args, flags);
	} else {
		pw_log_warn("ignoring unknown command `%s` with args `%s`",
				cmd, args);
	}
	if (res < 0) {
		if (flags && strstr(flags, "nofail")) {
			pw_log_info("nofail command %s %s: %s",
					cmd, args, spa_strerror(res));
			res = 0;
		} else {
			pw_log_error("can't run command %s %s: %s",
					cmd, args, spa_strerror(res));
		}
	}
	return res;
}

/*
 * pulse.cmd = [
 *   {   cmd = <command>
 *       ( args = "<arguments>" )
 *       ( flags = [ ( nofail ) ] )
 *   }
 *   ...
 * ]
 */
static int parse_cmd(void *user_data, const char *location,
		const char *section, const char *str, size_t len)
{
	struct impl *impl = user_data;
	struct spa_json it[3];
	char key[512], *s;
	int res = 0;

	s = strndup(str, len);
	spa_json_init(&it[0], s, len);
	if (spa_json_enter_array(&it[0], &it[1]) < 0) {
		pw_log_error("config file error: pulse.cmd is not an array");
		res = -EINVAL;
		goto exit;
	}

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char *cmd = NULL, *args = NULL, *flags = NULL;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			const char *val;
			int len;

			if ((len = spa_json_next(&it[2], &val)) <= 0)
				break;

			if (spa_streq(key, "cmd")) {
				cmd = (char*)val;
				spa_json_parse_stringn(val, len, cmd, len+1);
			} else if (spa_streq(key, "args")) {
				args = (char*)val;
				spa_json_parse_stringn(val, len, args, len+1);
			} else if (spa_streq(key, "flags")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);
				flags = (char*)val;
				spa_json_parse_stringn(val, len, flags, len+1);
			}
		}
		if (cmd != NULL)
			res = do_cmd(impl, cmd, args, flags);
		if (res < 0)
			break;
	}
exit:
	free(s);
	return res;
}

int cmd_run(struct impl *impl)
{
	return pw_context_conf_section_for_each(impl->context, "pulse.cmd",
				parse_cmd, impl);
}
