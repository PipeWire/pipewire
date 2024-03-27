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

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/debug/file.h>

static void encode_string(FILE *f, const char *val, int len)
{
	int i;
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

static int dump(FILE *file, int indent, struct spa_json *it, const char *value, int len)
{
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
			fprintf(file, "%s\n%*s", count++ > 0 ? "," : "",
					indent+2, "");
			if ((res = dump(file, indent+2, &sub, value, len)) < 0)
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
		while (spa_json_get_string(&sub, key, sizeof(key)) > 0) {
			fprintf(file, "%s\n%*s",
					count++ > 0 ? "," : "",
					indent+2, "");
			encode_string(file, key, strlen(key));
			fprintf(file, ": ");
			if ((len = spa_json_next(&sub, &value)) <= 0)
				break;
			res = dump(file, indent+2, &sub, value, len);
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
		encode_string(file, value, len);
	}

	if (spa_json_get_error(it, NULL, NULL))
		return -EINVAL;

	return 0;
}

static int process_json(const char *filename, void *buf, size_t size)
{
	int len, res;
	struct spa_json it;
	const char *value;

	spa_json_init(&it, buf, size);
	if ((len = spa_json_next(&it, &value)) <= 0) {
                fprintf(stderr, "not a valid file '%s': %s\n", filename, spa_strerror(len));
		return -EINVAL;
	}
	if (!spa_json_is_container(value, len)) {
		spa_json_init(&it, buf, size);
		value = NULL;
		len = 0;
	}
	res = dump(stdout, 0, &it, value, len);
	if (spa_json_next(&it, &value) < 0)
		res = -EINVAL;

	fprintf(stdout, "\n");
	fflush(stdout);

	if (res < 0) {
		struct spa_error_location loc;

		if (spa_json_get_error(&it, buf, &loc))
			spa_debug_file_error_location(stderr, &loc,
					"syntax error in file '%s': %s",
					filename, loc.reason);
		else
			fprintf(stderr, "error parsing file '%s': %s\n", filename, spa_strerror(res));

		return -EINVAL;
	}
	return 0;
}

static int process_stdin(void)
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

	err = process_json("-", buf, size);
	free(buf);

	return (err == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

error:
	free(buf);
	return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
	int fd, res, exit_code = EXIT_FAILURE;
	void *data;
	struct stat sbuf;

	if (argc < 1) {
		fprintf(stderr, "usage: %s [spa-json-file]\n", argv[0]);
		goto error;
	}
	if (argc == 1)
		return process_stdin();
	if ((fd = open(argv[1],  O_CLOEXEC | O_RDONLY)) < 0)  {
                fprintf(stderr, "error opening file '%s': %m\n", argv[1]);
		goto error;
        }
        if (fstat(fd, &sbuf) < 0) {
                fprintf(stderr, "error statting file '%s': %m\n", argv[1]);
                goto error_close;
	}
        if ((data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
                fprintf(stderr, "error mmapping file '%s': %m\n", argv[1]);
                goto error_close;
	}

	res = process_json(argv[1], data, sbuf.st_size);
	if (res < 0)
		exit_code = EXIT_FAILURE;
	else
		exit_code = EXIT_SUCCESS;

        munmap(data, sbuf.st_size);
error_close:
        close(fd);
error:
	return exit_code;
}
