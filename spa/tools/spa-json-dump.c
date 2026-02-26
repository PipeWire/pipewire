/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/json-builder.h>
#include <spa/debug/file.h>

#define DEFAULT_INDENT	2

struct data {
	const char *filename;

	FILE *out;
	struct spa_json_builder builder;

	void *data;
	size_t size;
};

#define OPTIONS		"hNC:Ri:s"
static const struct option long_options[] = {
	{ "help",	no_argument,		NULL, 'h'},

	{ "no-colors",	no_argument,		NULL, 'N' },
	{ "color",	optional_argument,	NULL, 'C' },
	{ "raw",	no_argument,		NULL, 'R' },
	{ "indent",	required_argument,	NULL, 'i' },
	{ "spa",	no_argument,		NULL, 's' },

	{ NULL, 0, NULL, 0 }
};

static void show_usage(struct data *d, const char *name, bool is_error)
{
	FILE *fp;

	fp = is_error ? stderr : stdout;

	fprintf(fp, "%s [options] [spa-json-file]\n", name);
	fprintf(fp,
		"  -h, --help                            Show this help\n"
		"\n");
	fprintf(fp,
		"  -N, --no-colors                       disable color output\n"
		"  -C, --color[=WHEN]                    whether to enable color support. WHEN is `never`, `always`, or `auto`\n"
		"  -R, --raw                             force raw output\n"
		"  -i  --indent                          set indent (default %d)\n"
		"  -s  --spa                             use simplified SPA JSON\n"
		"\n",
		DEFAULT_INDENT);
}

static int dump(struct data *d, struct spa_json *it, const char *key, const char *value, int len)
{
	struct spa_json_builder *b = &d->builder;
	struct spa_json sub;
	bool toplevel = false;
	int res;

	if (!value) {
		toplevel = true;
		value = "{";
		len = 1;
	}

	if (spa_json_is_array(value, len)) {
		spa_json_builder_object_push(b, key, "[");
		spa_json_enter(it, &sub);
		while ((len = spa_json_next(&sub, &value)) > 0) {
			if ((res = dump(d, &sub, NULL, value, len)) < 0)
				return res;
		}
		spa_json_builder_pop(b, "]");
	} else if (spa_json_is_object(value, len)) {
		char k[1024];
		spa_json_builder_object_push(b, key, "{");
		if (!toplevel)
			spa_json_enter(it, &sub);
		else
			sub = *it;
		while ((len = spa_json_object_next(&sub, k, sizeof(k), &value)) > 0) {
			res = dump(d, &sub, k, value, len);
			if (res < 0) {
				if (toplevel)
					*it = sub;
				return res;
			}
		}
		if (toplevel)
			*it = sub;
		spa_json_builder_pop(b, "}");
	} else {
		spa_json_builder_add_simple(b, key, INT_MAX, 0, value, len);
	}
	if (spa_json_get_error(it, NULL, NULL))
		return -EINVAL;

	return 0;
}

static int process_json(struct data *d)
{
	int len, res;
	struct spa_json it;
	const char *value;

	if ((len = spa_json_begin(&it, d->data, d->size, &value)) <= 0) {
		fprintf(stderr, "not a valid file '%s': %s\n", d->filename, spa_strerror(len));
		return -EINVAL;
	}
	if (!spa_json_is_container(value, len)) {
		spa_json_init(&it, d->data, d->size);
		value = NULL;
		len = 0;
	}

	res = dump(d, &it, NULL, value, len);
	if (spa_json_next(&it, &value) < 0)
		res = -EINVAL;

	fprintf(d->builder.f, "\n");
	fflush(d->builder.f);

	if (res < 0) {
		struct spa_error_location loc;

		if (spa_json_get_error(&it, d->data, &loc))
			spa_debug_file_error_location(stderr, &loc,
					"syntax error in file '%s': %s",
					d->filename, loc.reason);
		else
			fprintf(stderr, "error parsing file '%s': %s\n",
					d->filename, spa_strerror(res));

		return -EINVAL;
	}
	return 0;
}

static int process_stdin(struct data *d)
{
	uint8_t *buf = NULL, *p;
	size_t alloc = 0, size = 0, read_size, res;
	int err;

	do {
		alloc += 1024 + alloc;
		p = realloc(buf, alloc);
		if (!p) {
			fprintf(stderr, "error: %m\n");
			goto error;
		}
		buf = p;

		read_size = alloc - size;
		res = fread(buf + size, 1, read_size, stdin);
		size += res;
	} while (res == read_size);

	if (ferror(stdin)) {
		fprintf(stderr, "error: %m\n");
		goto error;
	}
	d->data = buf;
	d->size = size;

	err = process_json(d);
	free(buf);

	return (err == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

error:
	free(buf);
	return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
	int c;
	int longopt_index = 0;
	int fd, res, exit_code = EXIT_FAILURE;
	int flags = 0, indent = -1;
	struct data d;
	struct stat sbuf;
	bool raw = false, colors = false;

	spa_zero(d);

	d.filename = "-";
	d.out = stdout;

	if (getenv("NO_COLOR") == NULL && isatty(fileno(d.out)))
		colors = true;
	setlinebuf(d.out);

	while ((c = getopt_long(argc, argv, OPTIONS, long_options, &longopt_index)) != -1) {
		switch (c) {
		case 'h' :
			show_usage(&d, argv[0], false);
			return 0;
		case 'N' :
			colors = false;
			break;
		case 'C' :
			if (optarg == NULL || !strcmp(optarg, "auto"))
				break; /* nothing to do, tty detection was done
					  before parsing options */
			else if (!strcmp(optarg, "never"))
				colors = false;
			else if (!strcmp(optarg, "always"))
				colors = true;
			else {
				fprintf(stderr, "Unknown color: %s\n", optarg);
				show_usage(&d, argv[0], true);
				return -1;
			}
			break;
		case 'R':
			raw = true;
			break;
		case 'i':
			indent = atoi(optarg);
			break;
		case 's':
			flags |= SPA_JSON_BUILDER_FLAG_SIMPLE;
			break;
		default:
			show_usage(&d, argv[0], true);
			return -1;
		}
	}

	if (optind < argc)
		d.filename = argv[optind++];

	if (spa_streq(d.filename, "-"))
		return process_stdin(&d);

	if ((fd = open(d.filename,  O_CLOEXEC | O_RDONLY)) < 0) {
		fprintf(stderr, "error opening file '%s': %m\n", d.filename);
		goto error;
	}
	if (fstat(fd, &sbuf) < 0) {
		fprintf(stderr, "error statting file '%s': %m\n", d.filename);
		goto error_close;
	}
	if ((d.data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		fprintf(stderr, "error mmapping file '%s': %m\n", d.filename);
		goto error_close;
	}
	d.size = sbuf.st_size;

	if (!raw)
		flags |= SPA_JSON_BUILDER_FLAG_PRETTY;
	if (colors)
		flags |= SPA_JSON_BUILDER_FLAG_COLOR;

	spa_json_builder_file(&d.builder, d.out, flags);
	if (indent >= 0)
		d.builder.indent = indent;

	res = process_json(&d);
	if (res < 0)
		exit_code = EXIT_FAILURE;
	else
		exit_code = EXIT_SUCCESS;

	munmap(d.data, sbuf.st_size);
error_close:
	close(fd);
error:
	return exit_code;
}
