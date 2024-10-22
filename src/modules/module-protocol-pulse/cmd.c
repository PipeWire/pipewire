/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/cleanup.h>
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

	n = args != NULL ? pw_split_ip(args, WHITESPACE, 2, a) : 0;
	if (n < 1) {
		pw_log_info("load-module expects module name got '%s'", args);
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
	char key[512];
	int res = 0;

	spa_autofree char *s = strndup(str, len);
	if (spa_json_begin_array(&it[0], s, len) < 0) {
		pw_log_error("config file error: pulse.cmd is not an array");
		return -EINVAL;
	}

	while (spa_json_enter_object(&it[0], &it[1]) > 0) {
		char *cmd = NULL, *args = NULL, *flags = NULL;
		const char *val;
		bool have_match = true;
		int l;

		while ((l = spa_json_object_next(&it[1], key, sizeof(key), &val)) > 0) {
			if (spa_streq(key, "cmd")) {
				cmd = (char*)val;
				spa_json_parse_stringn(val, l, cmd, l+1);
			} else if (spa_streq(key, "args")) {
				args = (char*)val;
				spa_json_parse_stringn(val, l, args, l+1);
			} else if (spa_streq(key, "flags")) {
				if (spa_json_is_container(val, l))
					l = spa_json_container_len(&it[1], val, l);
				flags = (char*)val;
				spa_json_parse_stringn(val, l, flags, l+1);
			} else if (spa_streq(key, "condition")) {
				if (!spa_json_is_array(val, l)) {
					pw_log_warn("expected array for condition in '%.*s'",
							(int)l, str);
					break;
				}
				spa_json_enter(&it[1], &it[2]);
				have_match = pw_conf_find_match(&it[2], &impl->props->dict, true);
			} else {
				pw_log_warn("unknown pulse.cmd key %s", key);
			}
		}
		if (!have_match)
			continue;

		if (cmd != NULL)
			res = do_cmd(impl, cmd, args, flags);
		if (res < 0)
			break;
	}
	return res;
}

int cmd_run(struct impl *impl)
{
	return pw_context_conf_section_for_each(impl->context, "pulse.cmd",
				parse_cmd, impl);
}
