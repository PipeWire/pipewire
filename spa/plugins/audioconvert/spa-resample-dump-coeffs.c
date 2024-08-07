/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2024 Arun Raghavan <arun@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <spa/support/log-impl.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

SPA_LOG_IMPL(logger);

#include "resample.h"
#include "resample-native-impl.h"

#define OPTIONS		"ht:"
static const struct option long_options[] = {
	{ "help",	no_argument,		NULL, 'h'},

	{ "tuple",	required_argument,	NULL, 't' },

        { NULL, 0, NULL, 0 }
};

static void show_usage(const char *name, bool is_error)
{
	FILE *fp;

	fp = is_error ? stderr : stdout;

	fprintf(fp, "%s [options]\n", name);
	fprintf(fp,
		"  -h, --help                            Show this help\n"
		"\n"
		"  -t  --tuple                            Sample rate tuple (as \"in_rate,out_rate[,quality]\")\n"
		"\n");
}

static void parse_tuple(const char *arg, int *in, int *out, int *quality)
{
	char tuple[256];
	char *token;

	strncpy(tuple, arg, sizeof(tuple) - 1);
	*in = 0;
	*out = 0;

	token = strtok(tuple, ",");
	if (!token || !spa_atoi32(token, in, 10))
		return;

	token = strtok(NULL, ",");
	if (!token || !spa_atoi32(token, out, 10))
		return;

	token = strtok(NULL, ",");
	if (!token) {
		*quality = RESAMPLE_DEFAULT_QUALITY;
	} else if (!spa_atoi32(token, quality, 10)) {
		*quality = -1;
		return;
	}

	/* first, second now contain zeroes on error, or the numbers on success,
	 * third contains a quality or -1 on error, default value if unspecified */
}

#define PREFIX "__precomp_coeff"

static void dump_header(void)
{
	printf("/* This is a generated file, see spa-resample-dump-coeffs.c */");
	printf("\n#include <stdint.h>\n");
	printf("\n#include <stdlib.h>\n");
	printf("\n");
	printf("struct resample_coeffs {\n");
	printf("\tuint32_t in_rate;\n");
	printf("\tuint32_t out_rate;\n");
	printf("\tint quality;\n");
	printf("\tconst float *filter;\n");
	printf("};\n");
}

static void dump_footer(const uint32_t *ins, const uint32_t *outs, const int *qualities)
{
	printf("\n");
	printf("static const struct resample_coeffs precomp_coeffs[] = {\n");
	while (*ins && *outs) {
		printf("\t{ .in_rate = %u, .out_rate = %u, .quality = %u, "
				".filter = %s_%u_%u_%u },\n",
				*ins, *outs, *qualities, PREFIX, *ins, *outs, *qualities);
		ins++;
		outs++;
		qualities++;
	}
	printf("\t{ .in_rate = 0, .out_rate = 0, .quality = 0, .filter = NULL },\n");
	printf("};\n");
}

static void dump_coeffs(unsigned int in_rate, unsigned int out_rate, int quality)
{
	struct resample r = { 0, };
	struct native_data *d;
	unsigned int i, filter_size;
	int ret;

	r.log = &logger.log;
	r.i_rate = in_rate;
	r.o_rate = out_rate;
	r.quality = quality;
	r.channels = 1; /* irrelevant for generated taps */

	if ((ret = resample_native_init(&r)) < 0) {
		fprintf(stderr, "can't init converter: %s\n", spa_strerror(ret));
		return;
	}

	d = r.data;
	filter_size = d->filter_stride * (d->n_phases + 1);

	printf("\n");
	printf("static const float %s_%u_%u_%u[] = {", PREFIX, in_rate, out_rate, quality);
	for (i = 0; i < filter_size; i++) {
		printf("%a", d->filter[i]);
		if (i != filter_size - 1)
			printf(",");
	}
	printf("};\n");

	if (r.free)
		r.free(&r);
}

int main(int argc, char* argv[])
{
	unsigned int ins[256] = { 0, }, outs[256] = { 0, };
	int qualities[256] = { 0, };
	int in_rate = 0, out_rate = 0, quality = 0;
	int c, longopt_index = 0, i = 0;

	while ((c = getopt_long(argc, argv, OPTIONS, long_options, &longopt_index)) != -1) {
		switch (c) {
		case 'h':
                        show_usage(argv[0], false);
                        return EXIT_SUCCESS;
		case 't':
			parse_tuple(optarg, &in_rate, &out_rate, &quality);
			if (in_rate <= 0) {
				fprintf(stderr, "error: bad input rate %d\n", in_rate);
                                goto error;
			}
			if (out_rate <= 0) {
				fprintf(stderr, "error: bad output rate %d\n", out_rate);
                                goto error;
			}
			if (quality < 0 || quality > 14) {
				fprintf(stderr, "error: bad quality value %s\n", optarg);
                                goto error;
			}
			ins[i] = in_rate;
			outs[i] = out_rate;
			qualities[i] = quality;
			i++;
			break;
		default:
			fprintf(stderr, "error: unknown option\n");
			goto error_usage;
		}
	}

	if (optind != argc) {
                fprintf(stderr, "error: got %d extra argument(s))\n",
				optind - argc);
		goto error_usage;
	}
	if (in_rate == 0) {
                fprintf(stderr, "error: input rate must be specified\n");
		goto error;
	}
	if (out_rate == 0) {
                fprintf(stderr, "error: input rate must be specified\n");
		goto error;
	}

	dump_header();
	while (i--) {
		dump_coeffs(ins[i], outs[i], qualities[i]);
	}
	dump_footer(ins, outs, qualities);

	return EXIT_SUCCESS;

error_usage:
	show_usage(argv[0], true);
error:
	return EXIT_FAILURE;
}
