/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>

#include <sndfile.h>

SPA_LOG_IMPL(logger);

#include "resample.h"

#define DEFAULT_QUALITY	RESAMPLE_DEFAULT_QUALITY

#define MAX_SAMPLES	4096u

struct data {
	bool verbose;
	int rate;
	int format;
	uint32_t window;
	int quality;
	struct resample_config config;
	int cpu_flags;

	const char *iname;
	SF_INFO iinfo;
	SNDFILE *ifile;

	const char *oname;
	SF_INFO oinfo;
	SNDFILE *ofile;
};

#define STR_FMTS "(s8|s16|s32|f32|f64)"

#define OPTIONS		"hvc:r:f:w:q:u:t:p:"
static const struct option long_options[] = {
	{ "help",	no_argument,		NULL, 'h'},
	{ "verbose",	no_argument,		NULL, 'v'},
	{ "cpuflags",	required_argument,	NULL, 'c' },

	{ "rate",	required_argument,	NULL, 'r' },
	{ "format",	required_argument,	NULL, 'f' },
	{ "window",	required_argument,	NULL, 'w' },
	{ "quality",	required_argument,	NULL, 'q' },
	{ "cutoff",	required_argument,	NULL, 'u' },
	{ "taps",	required_argument,	NULL, 't' },
	{ "param",	required_argument,	NULL, 'p' },

        { NULL, 0, NULL, 0 }
};

static void show_usage(const char *name, bool is_error)
{
	FILE *fp;
	uint32_t i;

	fp = is_error ? stderr : stdout;

	fprintf(fp, "%s [options] <infile> <outfile>\n", name);
	fprintf(fp,
		"  -h, --help                            Show this help\n"
		"  -v  --verbose                         Be verbose\n"
		"  -c  --cpuflags                        CPU flags (default 0)\n"
		"\n");
	fprintf(fp,
		"  -r  --rate                            Output sample rate (default as input)\n"
		"  -f  --format                          Output sample format %s (default as input)\n\n"
		"  -w  --window                          Window function (default %s)\n",
		STR_FMTS, resample_window_name(RESAMPLE_WINDOW_DEFAULT));
	for (i = 0; i < SPA_N_ELEMENTS(resample_window_info); i++) {
		fprintf(fp,
			"                                                %s: %s\n",
			resample_window_info[i].label,
			resample_window_info[i].description);
	}
	fprintf(fp,
		"  -q  --quality                         Resampler quality (default %u)\n"
		"  -u  --cutoff                          Cutoff frequency [0.0..1.0] (default from quality)\n"
		"  -t  --taps                            Resampler taps (default from quality)\n"
		"  -p  --param                           Resampler param <name>=<value> (default from quality)\n",
		DEFAULT_QUALITY);
	for (i = 0; i < SPA_N_ELEMENTS(resample_param_info); i++) {
		fprintf(fp,
			"                                                %s\n",
			resample_param_info[i].label);
	}
	fprintf(fp, "\n");
}

static inline const char *
sf_fmt_to_str(int fmt)
{
	switch(fmt & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_S8:
		return "s8";
	case SF_FORMAT_PCM_16:
		return "s16";
	case SF_FORMAT_PCM_24:
		return "s24";
	case SF_FORMAT_PCM_32:
		return "s32";
	case SF_FORMAT_FLOAT:
		return "f32";
	case SF_FORMAT_DOUBLE:
		return "f64";
	default:
		return "unknown";
	}
}

static inline int
sf_str_to_fmt(const char *str)
{
	if (!str)
		return -1;
	if (spa_streq(str, "s8"))
		return SF_FORMAT_PCM_S8;
	if (spa_streq(str, "s16"))
		return SF_FORMAT_PCM_16;
	if (spa_streq(str, "s24"))
		return SF_FORMAT_PCM_24;
	if (spa_streq(str, "s32"))
		return SF_FORMAT_PCM_32;
	if (spa_streq(str, "f32"))
		return SF_FORMAT_FLOAT;
	if (spa_streq(str, "f64"))
		return SF_FORMAT_DOUBLE;
	return -1;
}

static int open_files(struct data *d)
{
        int i, count = 0, format = -1;

	d->ifile = sf_open(d->iname, SFM_READ, &d->iinfo);
        if (d->ifile == NULL) {
		fprintf(stderr, "error: failed to open input file \"%s\": %s\n",
				d->iname, sf_strerror(NULL));
		return -EIO;
	}

	d->oinfo.channels = d->iinfo.channels;
	d->oinfo.samplerate = d->rate > 0 ? d->rate : d->iinfo.samplerate;
	d->oinfo.format = d->format > 0 ? d->format : (d->iinfo.format & SF_FORMAT_SUBMASK);

	/* try to guess the format from the extension */
	if (sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof(int)) != 0)
		count = 0;

	for (i = 0; i < count; i++) {
		SF_FORMAT_INFO fi;

		spa_zero(fi);
		fi.format = i;
		if (sf_command(NULL, SFC_GET_FORMAT_MAJOR, &fi, sizeof(fi)) != 0)
			continue;

		if (spa_strendswith(d->oname, fi.extension)) {
			format = fi.format;
			break;
		}
	}
	if (format == -1)
		/* use the same format as the input file otherwise */
		format = d->iinfo.format & ~SF_FORMAT_SUBMASK;
	if (format == SF_FORMAT_WAV && d->oinfo.channels > 2)
		format = SF_FORMAT_WAVEX;

	d->oinfo.format |= format;

	d->ofile = sf_open(d->oname, SFM_WRITE, &d->oinfo);
        if (d->ofile == NULL) {
		fprintf(stderr, "error: failed to open output file \"%s\": %s\n",
				d->oname, sf_strerror(NULL));
		return -EIO;
	}
	if (d->verbose) {
		fprintf(stdout, "input '%s': channels:%d rate:%d format:%s\n",
				d->iname, d->iinfo.channels, d->iinfo.samplerate,
				sf_fmt_to_str(d->iinfo.format));
		fprintf(stdout, "output '%s': channels:%d rate:%d format:%s\n",
				d->oname, d->oinfo.channels, d->oinfo.samplerate,
				sf_fmt_to_str(d->oinfo.format));
	}
	return 0;
}

static int close_files(struct data *d)
{
	if (d->ifile)
		sf_close(d->ifile);
	if (d->ofile)
		sf_close(d->ofile);
	return 0;
}

static int do_conversion(struct data *d)
{
	struct resample r;
	int channels = d->iinfo.channels;
	float in[MAX_SAMPLES * channels];
	float out[MAX_SAMPLES * channels];
	float ibuf[MAX_SAMPLES * channels];
	float obuf[MAX_SAMPLES * channels];
	uint32_t in_len, out_len, queued;
	uint32_t pin_len, pout_len;
	size_t read, written;
	const void *src[channels];
	void *dst[channels];
	uint32_t i;
	int res, j, k;
	uint32_t flushing = UINT32_MAX;

	spa_zero(r);
	r.cpu_flags = d->cpu_flags;
	r.log = &logger.log;
	r.channels = channels;
	r.i_rate = d->iinfo.samplerate;
	r.o_rate = d->oinfo.samplerate;
	r.quality = d->quality < 0 ? DEFAULT_QUALITY : d->quality;
	r.config = d->config;
	if ((res = resample_native_init(&r)) < 0) {
		fprintf(stderr, "can't init converter: %s\n", spa_strerror(res));
		return res;
	}
	if (d->verbose) {
		fprintf(stdout, "window:%s cutoff:%f n_taps:%u\n",
				resample_window_name(r.config.window),
				r.config.cutoff, r.config.n_taps);
		for (i = 0; i < SPA_N_ELEMENTS(resample_param_info); i++) {
			if (resample_param_info[i].window != r.config.window)
				continue;
			fprintf(stdout, "  param:%s %f\n",
				resample_param_info[i].label,
				r.config.params[resample_param_info[i].idx]);
		}
	}

	for (j = 0; j < channels; j++)
		src[j] = &in[MAX_SAMPLES * j];
	for (j = 0; j < channels; j++)
		dst[j] = &out[MAX_SAMPLES * j];

	read = written = queued = 0;
	while (true) {
		pout_len = out_len = MAX_SAMPLES;
		in_len = SPA_MIN(MAX_SAMPLES, resample_in_len(&r, out_len));
		in_len -= SPA_MIN(queued, in_len);

		if (in_len > 0) {
			pin_len = in_len = sf_readf_float(d->ifile, &ibuf[queued * channels], in_len);

			read += pin_len;

			if (pin_len == 0) {
				if (flushing == 0)
					break;
				if (flushing == UINT32_MAX)
					flushing = resample_delay(&r);

				pin_len = in_len = SPA_MIN(MAX_SAMPLES, flushing);
				flushing -= in_len;

				for (k = 0, i = 0; i < pin_len; i++) {
					for (j = 0; j < channels; j++)
						ibuf[k++] = 0.0;
				}
			}
		}
		in_len += queued;
		pin_len = in_len;

		for (k = 0, i = 0; i < pin_len; i++) {
			for (j = 0; j < channels; j++) {
				in[MAX_SAMPLES * j + i] = ibuf[k++];
			}
		}
                resample_process(&r, src, &pin_len, dst, &pout_len);

		queued = in_len - pin_len;
		if (queued)
			memmove(ibuf, &ibuf[pin_len * channels], queued * channels * sizeof(float));

		if (pout_len > 0) {
			for (k = 0, i = 0; i < pout_len; i++) {
				for (j = 0; j < channels; j++) {
					obuf[k++] = out[MAX_SAMPLES * j + i];
				}
			}
			pout_len = sf_writef_float(d->ofile, obuf, pout_len);

			written += pout_len;
		}
	}
	if (d->verbose)
		fprintf(stdout, "read %zu samples, wrote %zu samples\n", read, written);

	return 0;
}

int main(int argc, char *argv[])
{
	int c;
	int longopt_index = 0, ret;
	struct data data;

	spa_zero(data);

	logger.log.level = SPA_LOG_LEVEL_DEBUG;

	data.quality = -1;
	while ((c = getopt_long(argc, argv, OPTIONS, long_options, &longopt_index)) != -1) {
		switch (c) {
		case 'h':
                        show_usage(argv[0], false);
                        return EXIT_SUCCESS;
		case 'v':
			data.verbose = true;
			break;
		case 'r':
			ret = atoi(optarg);
			if (ret <= 0) {
				fprintf(stderr, "error: bad rate %s\n", optarg);
                                goto error_usage;
			}
			data.rate = ret;
			break;
		case 'f':
			ret = sf_str_to_fmt(optarg);
			if (ret < 0) {
				fprintf(stderr, "error: bad format %s\n", optarg);
                                goto error_usage;
			}
			data.format = ret;
			break;
		case 'q':
			ret = atoi(optarg);
			if (ret < 0) {
				fprintf(stderr, "error: bad quality %s\n", optarg);
                                goto error_usage;
			}
			data.quality = ret;
			break;
		case 'c':
			data.cpu_flags = strtol(optarg, NULL, 0);
			break;
		case 'u':
			data.config.cutoff = strtod(optarg, NULL);
			fprintf(stderr, "%f\n", data.config.cutoff);
			break;
		case 'w':
			data.config.window = resample_window_from_label(optarg);
			break;
		case 'p':
		{
			char *eq;
			if ((eq = strchr(optarg, '=')) != NULL) {
				uint32_t idx;
				*eq = 0;
				idx = resample_param_from_label(optarg);
				data.config.params[idx] = atof(eq+1);
			}
			break;
		}
		case 't':
			data.config.n_taps = atoi(optarg);
			break;
                default:
			fprintf(stderr, "error: unknown option '%c'\n", c);
			goto error_usage;
		}
	}
	if (optind + 1 >= argc) {
                fprintf(stderr, "error: filename arguments missing (%d %d)\n", optind, argc);
		goto error_usage;
	}
        data.iname = argv[optind++];
        data.oname = argv[optind++];

	if (open_files(&data) < 0)
		return EXIT_FAILURE;

	do_conversion(&data);

	close_files(&data);

	return 0;

error_usage:
        show_usage(argv[0], true);
	return EXIT_FAILURE;
}
