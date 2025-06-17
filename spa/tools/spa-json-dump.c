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
#include <spa/debug/file.h>

#define DEFAULT_INDENT	2

struct data {
	const char *filename;
	FILE *file;

	void *data;
	size_t size;

	int indent;
	bool simple_string;
	const char *comma;
	const char *key_sep;
};

#define OPTIONS		"hi:s"
static const struct option long_options[] = {
	{ "help",	no_argument,		NULL, 'h'},

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
		"  -i  --indent                          set indent (default %d)\n"
		"  -s  --spa                             use simplified SPA JSON\n"
		"\n",
		DEFAULT_INDENT);
}

#define REJECT	"\"\\'=:,{}[]()#"

static bool is_simple_string(const char *val, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		if (val[i] < 0x20 || strchr(REJECT, val[i]) != NULL)
			return false;
	}
	return true;
}

static void encode_string(struct data *d, const char *val, int len)
{
	FILE *f = d->file;
	int i;
	if (d->simple_string && is_simple_string(val, len)) {
		fprintf(f, "%.*s", len, val);
		return;
	}
	fprintf(f, "\"");
	for (i = 0; i < len; i++) {
		char v = val[i];
		switch (v) {
		case '\n':
			fprintf(f, "\\n");
			break;
		case '\r':
			fprintf(f, "\\r");
			break;
		case '\b':
			fprintf(f, "\\b");
			break;
		case '\t':
			fprintf(f, "\\t");
			break;
		case '\f':
			fprintf(f, "\\f");
			break;
		case '\\': case '"':
			fprintf(f, "\\%c", v);
			break;
		default:
			if (v > 0 && v < 0x20)
				fprintf(f, "\\u%04x", v);
			else
				fprintf(f, "%c", v);
			break;
		}
	}
	fprintf(f, "\"");
}

static int dump(struct data *d, int indent, struct spa_json *it, const char *value, int len)
{
	FILE *file = d->file;
	struct spa_json sub;
	bool toplevel = false;
	int count = 0, res;
	char key[1024];

	if (!value) {
		toplevel = true;
		value = "{";
		len = 1;
	}

	if (spa_json_is_array(value, len)) {
		fprintf(file, "[");
		spa_json_enter(it, &sub);
		while ((len = spa_json_next(&sub, &value)) > 0) {
			fprintf(file, "%s\n%*s", count++ > 0 ? d->comma : "",
					indent+d->indent, "");
			if ((res = dump(d, indent+d->indent, &sub, value, len)) < 0)
				return res;
		}
		fprintf(file, "%s%*s]", count > 0 ? "\n" : "",
				count > 0 ? indent : 0, "");
	} else if (spa_json_is_object(value, len)) {
		fprintf(file, "{");
		if (!toplevel)
			spa_json_enter(it, &sub);
		else
			sub = *it;
		while ((len = spa_json_object_next(&sub, key, sizeof(key), &value)) > 0) {
			fprintf(file, "%s\n%*s",
					count++ > 0 ? d->comma : "",
					indent+d->indent, "");
			encode_string(d, key, strlen(key));
			fprintf(file, "%s ", d->key_sep);
			res = dump(d, indent+d->indent, &sub, value, len);
			if (res < 0) {
				if (toplevel)
					*it = sub;
				return res;
			}
		}
		if (toplevel)
			*it = sub;
		fprintf(file, "%s%*s}", count > 0 ? "\n" : "",
				count > 0 ? indent : 0, "");
	} else if (spa_json_is_string(value, len) ||
	    spa_json_is_null(value, len) ||
	    spa_json_is_bool(value, len) ||
	    spa_json_is_int(value, len) ||
	    spa_json_is_float(value, len)) {
		fprintf(file, "%.*s", len, value);
	} else {
		encode_string(d, value, len);
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

	res = dump(d, 0, &it, value, len);
	if (spa_json_next(&it, &value) < 0)
		res = -EINVAL;

	fprintf(d->file, "\n");
	fflush(d->file);

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
	struct data d;
	struct stat sbuf;

	spa_zero(d);
	d.file = stdout;

	d.filename = "-";
	d.simple_string = false;
	d.comma = ",";
	d.key_sep = ":";
	d.indent = DEFAULT_INDENT;

	while ((c = getopt_long(argc, argv, OPTIONS, long_options, &longopt_index)) != -1) {
		switch (c) {
		case 'h' :
			show_usage(&d, argv[0], false);
			return 0;
		case 'i':
			d.indent = atoi(optarg);
			break;
		case 's':
			d.simple_string = true;
			d.comma = "";
			d.key_sep = " =";
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

	 ((fd = open(d.filename,  O_CLOEXEC | O_RDONLY)) < 0)  {
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
