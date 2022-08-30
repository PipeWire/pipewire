/* Spa
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

#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <getopt.h>
#include <math.h>

#include <alsa/asoundlib.h>

#include <spa/utils/defs.h>

#define DEFAULT_DEVICE	"default"


struct state {
	const char *device;
	snd_output_t *output;
	snd_pcm_t *hndl;
};

#define CHECK(s,msg,...) {		\
	int __err;			\
	if ((__err = (s)) < 0) {	\
		fprintf(stderr, msg ": %s\n", ##__VA_ARGS__, snd_strerror(__err));	\
		return __err;		\
	}				\
}

static const char *get_class(snd_pcm_class_t c)
{
	switch (c) {
	case SND_PCM_CLASS_GENERIC:
		return "generic";
	case SND_PCM_CLASS_MULTI:
		return "multichannel";
	case SND_PCM_CLASS_MODEM:
		return "modem";
	case SND_PCM_CLASS_DIGITIZER:
		return "digitizer";
	default:
		return "unknown";
	}
}

static const char *get_subclass(snd_pcm_subclass_t c)
{
	switch (c) {
	case SND_PCM_SUBCLASS_GENERIC_MIX:
		return "generic-mix";
	case SND_PCM_SUBCLASS_MULTI_MIX:
		return "multichannel-mix";
	default:
		return "unknown";
	}
}

static void show_help(const char *name, bool error)
{
        fprintf(error ? stderr : stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"  -D, --device                          device name (default '%s')\n"
		"  -C, --capture                         capture mode (default playback)\n",
		name, DEFAULT_DEVICE);
}

int main(int argc, char *argv[])
{
	struct state state = { 0, };
	snd_pcm_hw_params_t *hparams;
	snd_pcm_info_t *info;
	snd_pcm_sync_id_t sync;
	snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
	snd_pcm_chmap_query_t **maps;
	int c, i;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "device",	required_argument,	NULL, 'D' },
		{ "capture",	no_argument,		NULL, 'C' },
		{ NULL, 0, NULL, 0}
	};
	state.device = DEFAULT_DEVICE;

	while ((c = getopt_long(argc, argv, "hD:C", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0], false);
			return 0;
		case 'D':
			state.device = optarg;
			break;
		case 'C':
			stream = SND_PCM_STREAM_CAPTURE;
			break;
		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	CHECK(snd_output_stdio_attach(&state.output, stdout, 0), "attach failed");

	fprintf(stdout, "opening device: '%s'\n", state.device);

	CHECK(snd_pcm_open(&state.hndl, state.device, stream, 0),
			"open %s failed", state.device);

	snd_pcm_info_alloca(&info);
	snd_pcm_info(state.hndl, info);

	fprintf(stdout, "info:\n");
	fprintf(stdout, "  device: %u\n", snd_pcm_info_get_device(info));
	fprintf(stdout, "  subdevice: %u\n", snd_pcm_info_get_subdevice(info));
	fprintf(stdout, "  stream: %s\n", snd_pcm_stream_name(snd_pcm_info_get_stream(info)));
	fprintf(stdout, "  card: %d\n", snd_pcm_info_get_card(info));
	fprintf(stdout, "  id: '%s'\n", snd_pcm_info_get_id(info));
	fprintf(stdout, "  name: '%s'\n", snd_pcm_info_get_name(info));
	fprintf(stdout, "  subdevice name: '%s'\n", snd_pcm_info_get_subdevice_name(info));
	fprintf(stdout, "  class: %s\n", get_class(snd_pcm_info_get_class(info)));
	fprintf(stdout, "  subclass: %s\n", get_subclass(snd_pcm_info_get_subclass(info)));
	fprintf(stdout, "  subdevice count: %u\n", snd_pcm_info_get_subdevices_count(info));
	fprintf(stdout, "  subdevice avail: %u\n", snd_pcm_info_get_subdevices_avail(info));
	sync = snd_pcm_info_get_sync(info);
	fprintf(stdout, "  sync: %08x:%08x:%08x:%08x\n",
			sync.id32[0], sync.id32[1], sync.id32[2],sync.id32[3]);

	/* channel maps */
	if ((maps = snd_pcm_query_chmaps(state.hndl)) != NULL) {
		fprintf(stdout, "channels:\n");

		for (i = 0; maps[i]; i++) {
			snd_pcm_chmap_t* map = &maps[i]->map;
			char buf[2048];

			snd_pcm_chmap_print(map, sizeof(buf), buf);

			fprintf(stdout, "  %d: %s\n", map->channels, buf);
		}
		snd_pcm_free_chmaps(maps);
	}

	/* hw params */
	snd_pcm_hw_params_alloca(&hparams);
	snd_pcm_hw_params_any(state.hndl, hparams);

	snd_pcm_hw_params_dump(hparams, state.output);

	snd_pcm_close(state.hndl);

	return EXIT_SUCCESS;
}
