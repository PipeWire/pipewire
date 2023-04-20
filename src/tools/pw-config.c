/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <getopt.h>
#include <signal.h>
#include <locale.h>
#include <unistd.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include "pipewire/pipewire.h"
#include "pipewire/log.h"

#define DEFAULT_NAME	"pipewire.conf"
#define DEFAULT_PREFIX	""

struct data {
	const char *opt_name;
	const char *opt_prefix;
	const char *opt_cmd;
	bool opt_recurse;
	bool opt_newline;
	bool opt_colors;
	struct pw_properties *conf;
	struct pw_properties *assemble;
	int count;
	bool array;
};

static void print_all_properties(struct data *d, struct pw_properties *props)
{
	pw_properties_serialize_dict(stdout,
			&props->dict,
			(d->opt_newline ? PW_PROPERTIES_FLAG_NL : 0) |
			(d->opt_recurse ? PW_PROPERTIES_FLAG_RECURSE : 0) |
			(d->opt_colors ? PW_PROPERTIES_FLAG_COLORS : 0) |
			(d->array ? PW_PROPERTIES_FLAG_ARRAY : 0) |
			PW_PROPERTIES_FLAG_ENCLOSE);
	fprintf(stdout, "\n");
}

static void list_paths(struct data *d)
{
	const struct spa_dict_item *it;

	spa_dict_for_each(it, &d->conf->dict) {
		if (spa_strstartswith(it->key, "config.path")) {
			pw_properties_set(d->assemble, it->key, it->value);
		}
		if (spa_strstartswith(it->key, "override.") &&
		    spa_strendswith(it->key, ".config.path")) {
			pw_properties_set(d->assemble, it->key, it->value);
		}
	}
}

static int do_merge_section(void *data, const char *location, const char *section,
			const char *str, size_t len)
{
	struct data *d = data;
	struct spa_json it[2];
	int l;
	const char *value;

        spa_json_init(&it[0], str, len);
	if ((l = spa_json_next(&it[0], &value)) <= 0)
		return 0;

	if (spa_json_is_array(value, l)) {
		char key[128];

		spa_json_enter(&it[0], &it[1]);
		while ((l = spa_json_next(&it[1], &value)) > 0) {
			if (spa_json_is_container(value, l))
				l = spa_json_container_len(&it[1], value, l);

			snprintf(key, sizeof(key), "%d-%s", d->count++, location);
			pw_properties_setf(d->assemble, key, "%.*s", l, value);
		}
		d->array = true;
	}
	else if (spa_json_is_object(value, l)) {
		pw_properties_update_string(d->assemble, str, len);
	}
	return 0;
}

static int do_list_section(void *data, const char *location, const char *section,
			const char *str, size_t len)
{
	struct data *d = data;
	char key[128];
	snprintf(key, sizeof(key), "%d-%s", d->count++, location);
	pw_properties_setf(d->assemble, key, "%.*s", (int)len, str);
	return 0;
}

static void section_for_each(struct data *d, const char *section,
		int (*callback) (void *data, const char *location, const char *section,
			const char *str, size_t len))
{
	const char *str;
	char key[128];

	pw_conf_section_for_each(&d->conf->dict, section, callback, d);
	str = pw_properties_get(d->assemble, "config.ext");
	if (str != NULL) {
		snprintf(key, sizeof(key), "%s.%s", section, str);
		pw_conf_section_for_each(&d->conf->dict, key, callback, d);
	}
}

static void show_help(const char *name, bool error)
{
        fprintf(error ? stderr : stdout, "%1$s : PipeWire config manager.\n"
		"Usage:\n"
		"  %1$s [options] paths                  List config paths (default action)\n"
		"  %1$s [options] list [SECTION]         List config section\n"
		"  %1$s [options] merge SECTION          Merge a config section\n\n"
		"Options:\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -n, --name                            Config Name (default '%2$s')\n"
		"  -p, --prefix                          Config Prefix (default '%3$s')\n"
		"  -L, --no-newline                      Omit newlines after values\n"
		"  -r, --recurse                         Reformat config sections recursively\n"
		"  -N, --no-colors                       disable color output\n"
		"  -C, --color[=WHEN]                    whether to enable color support. WHEN is `never`, `always`, or `auto`\n",
		name, DEFAULT_NAME, DEFAULT_PREFIX);
}

int main(int argc, char *argv[])
{
	struct data d = { 0, };
	struct pw_properties *props = NULL;
	int res = 0, c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "name",	required_argument,	NULL, 'n' },
		{ "prefix",	required_argument,	NULL, 'p' },
		{ "no-newline",	no_argument,		NULL, 'L' },
		{ "recurse",	no_argument,		NULL, 'r' },
		{ "no-colors",	no_argument,		NULL, 'N' },
		{ "color",	optional_argument,	NULL, 'C' },
		{ NULL, 0, NULL, 0}
	};

	d.opt_name = DEFAULT_NAME;
	d.opt_prefix = NULL;
	d.opt_recurse = false;
	d.opt_newline = true;
	if (isatty(fileno(stdout)) && getenv("NO_COLOR") == NULL)
		d.opt_colors = true;
	d.opt_cmd = "paths";

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVn:p:LrNC", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0], false);
			return 0;
		case 'V':
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'n':
			d.opt_name = optarg;
			break;
		case 'p':
			d.opt_prefix = optarg;
			break;
		case 'L':
			d.opt_newline = false;
			break;
		case 'r':
			d.opt_recurse = true;
			break;
		case 'N' :
			d.opt_colors = false;
			break;
		case 'C' :
			if (optarg == NULL || !strcmp(optarg, "auto"))
				break; /* nothing to do, tty detection was done
					  before parsing options */
			else if (!strcmp(optarg, "never"))
				d.opt_colors = false;
			else if (!strcmp(optarg, "always"))
				d.opt_colors = true;
			else {
				fprintf(stderr, "Unknown color: %s\n", optarg);
				show_help(argv[0], true);
				return -1;
			}
			break;
		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	if (optind < argc)
		d.opt_cmd = argv[optind++];

	props = pw_properties_new(
			PW_KEY_CONFIG_NAME, d.opt_name,
			PW_KEY_CONFIG_PREFIX, d.opt_prefix,
			NULL);

	d.conf = pw_properties_new(NULL, NULL);
	if ((res = pw_conf_load_conf_for_context (props, d.conf)) < 0) {
		fprintf(stderr, "error loading config: %s\n", spa_strerror(res));
		goto done;
	}

	d.assemble = pw_properties_new(NULL, NULL);

	if (spa_streq(d.opt_cmd, "paths")) {
		list_paths(&d);
	}
	else if (spa_streq(d.opt_cmd, "list")) {
		if (optind == argc) {
			pw_properties_update(d.assemble, &d.conf->dict);
		} else {
			section_for_each(&d, argv[optind++], do_list_section);
		}
	}
	else if (spa_streq(d.opt_cmd, "merge")) {
		if (optind == argc) {
			fprintf(stderr, "%s requires a section\n", d.opt_cmd);
			res = -EINVAL;
			goto done;
		}
		section_for_each(&d, argv[optind++], do_merge_section);
	}
	print_all_properties(&d, d.assemble);

done:
	pw_properties_free(d.conf);
	pw_properties_free(d.assemble);
	pw_properties_free(props);

	pw_deinit();
	return res;
}
