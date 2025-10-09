/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2025 Arun Raghavan */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>

#include <sndfile.h>

#include <spa/debug/dict.h>
#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/support/plugin-loader.h>
#include <spa/support/system.h>
#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>

static SPA_LOG_IMPL(default_log);

struct data {
	const char *plugin_dir;

	struct spa_log *log;
	struct spa_system *system;
	struct spa_loop *loop;
	struct spa_loop_control *control;
	struct spa_loop_utils *loop_utils;
	struct spa_plugin_loader *plugin_loader;

	struct spa_support support[6];
	uint32_t n_support;

	struct spa_audio_aec *aec;
	struct spa_handle *aec_handle;
	uint32_t aec_samples;
};

static int load_handle(struct data *data, struct spa_handle **handle, const char *lib, const char *name)
{
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;

	char *path = NULL;

	if ((path = spa_aprintf("%s/%s", data->plugin_dir, lib)) == NULL) {
		return -ENOMEM;
	}
	if ((hnd = dlopen(path, RTLD_NOW)) == NULL) {
		printf("can't load %s: %s\n", path, dlerror());
		free(path);
		return -errno;
	}
	free(path);
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		return -errno;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (!spa_streq(factory->name, name))
			continue;

		*handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res = spa_handle_factory_init(factory, *handle,
						NULL, data->support,
						data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		return 0;
	}
	return -EBADF;
}

static int init(struct data *data)
{
	int res;
	const char *str;
	struct spa_handle *handle = NULL;
	void *iface;

	if ((str = getenv("SPA_PLUGIN_DIR")) == NULL)
		str = PLUGINDIR;
	data->plugin_dir = str;

	if ((res = load_handle(data, &handle,
					"support/libspa-support.so",
					SPA_NAME_SUPPORT_SYSTEM)) < 0)
		return res;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_System, &iface)) < 0) {
		printf("can't get System interface %d\n", res);
		return res;
	}
	data->system = iface;
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, data->system);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataSystem, data->system);

	if ((res = load_handle(data, &handle,
					"support/libspa-support.so",
					SPA_NAME_SUPPORT_LOOP)) < 0)
		return res;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Loop, &iface)) < 0) {
		printf("can't get interface %d\n", res);
		return res;
	}
	data->loop = iface;
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_LoopControl, &iface)) < 0) {
		printf("can't get interface %d\n", res);
		return res;
	}
	data->control = iface;
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_LoopUtils, &iface)) < 0) {
		printf("can't get interface %d\n", res);
		return res;
	}
	data->loop_utils = iface;

	data->log = &default_log.log;

	if ((str = getenv("SPA_DEBUG")))
		data->log->level = atoi(str);

	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, data->log);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, data->loop);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, data->loop);
	data->support[data->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_LoopUtils, data->loop_utils);

	/* Use webrtc as default */
	if ((res = load_handle(data, &handle,
					"aec/libspa-aec-webrtc.so",
					SPA_NAME_AEC)) < 0)
		return res;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_AUDIO_AEC, &iface)) < 0) {
		spa_log_error(data->log, "can't get %s interface %d", SPA_TYPE_INTERFACE_AUDIO_AEC, res);
		return res;
	}

	data->aec = iface;
	data->aec_handle = handle;

	return 0;
}

static int spa_dict_from_json(struct spa_dict_item *items, uint32_t n_items, const char *str)
{
	struct spa_json it;
	int res;
	char key[1024];
	const char *value;
	uint32_t i, len;
	struct spa_error_location loc;

	if ((res = spa_json_begin_object_relax(&it, str, strlen(str))) < 0) {
		return res;
	}

	i = 0;
	while ((len = spa_json_object_next(&it, key, sizeof(key), &value)) > 0) {
		if (i > n_items)
			return -ENOSPC;

		char *k = malloc(strlen(key) + 1);
		char *v = malloc(len + 1);

		memcpy(k, key, strlen(key) + 1);
		spa_json_parse_stringn(value, len, v, len + 1);

		items[i++] = SPA_DICT_ITEM_INIT(k, v);
	}

	if (spa_json_get_error(&it, str, &loc)) {
		struct spa_debug_context *c = NULL;
		spa_debugc(c, "Invalid JSON: %s", loc.reason);
		spa_debugc_error_location(c, &loc);
		return -EINVAL;
	}

	return i;
}

static const struct format_info {
	const char *name;
	int sf_format;
	uint32_t spa_format;
	uint32_t width;
} format_info[] = {
	{  "ulaw", SF_FORMAT_ULAW, SPA_AUDIO_FORMAT_ULAW, 1 },
	{  "alaw", SF_FORMAT_ULAW, SPA_AUDIO_FORMAT_ALAW, 1 },
	{  "s8", SF_FORMAT_PCM_S8, SPA_AUDIO_FORMAT_S8, 1 },
	{  "u8", SF_FORMAT_PCM_U8, SPA_AUDIO_FORMAT_U8, 1 },
	{  "s16", SF_FORMAT_PCM_16, SPA_AUDIO_FORMAT_S16, 2 },
	{  "s24", SF_FORMAT_PCM_24, SPA_AUDIO_FORMAT_S24, 3 },
	{  "s32", SF_FORMAT_PCM_32, SPA_AUDIO_FORMAT_S32, 4 },
	{  "f32", SF_FORMAT_FLOAT, SPA_AUDIO_FORMAT_F32, 4 },
	{  "f64", SF_FORMAT_DOUBLE, SPA_AUDIO_FORMAT_F32, 8 },
};

static SNDFILE* open_file_read(const struct data *data, const char *name, struct spa_audio_info_raw *info)
{
	SF_INFO sf_info = { 0, };

	SNDFILE *file = sf_open(name, SFM_READ, &sf_info);

	if (!file) {
		spa_log_error(data->log, "Could not open file: %s", sf_strerror(NULL));
		exit(255);
	}

	for (unsigned long i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if ((sf_info.format & SF_FORMAT_SUBMASK) == format_info[i].sf_format) {
			info->format = format_info[i].spa_format;
			break;
		}
	}

	info->rate = sf_info.samplerate;
	info->channels = sf_info.channels;

	return file;
}

static SNDFILE* open_file_write(const struct data *data, const char *name, struct spa_audio_info_raw *info)
{
	SF_INFO sf_info = { 0, };

	for (unsigned long i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (info->format == format_info[i].spa_format) {
			sf_info.format = SF_FORMAT_WAV | format_info[i].sf_format;
			break;
		}
	}

	sf_info.samplerate = info->rate;
	sf_info.channels = info->channels;

	SNDFILE *file = sf_open(name, SFM_WRITE, &sf_info);

	if (!file) {
		spa_log_error(data->log, "Could not open file: %s", sf_strerror(NULL));
		exit(255);
	}

	return file;
}

static void deinterleave(float *data, uint32_t channels, uint32_t samples)
{
	float temp[channels * samples];

	for (uint32_t i = 0; i < channels; i++) {
		for (uint32_t j = 0; j < samples; j++) {
			temp[i * samples + j] = data[j * channels + i];
		}
	}

	memcpy(data, temp, sizeof(temp));
}

static void interleave(float *data, uint32_t channels, uint32_t samples)
{
	float temp[channels * samples];

	for (uint32_t i = 0; i < samples; i++) {
		for (uint32_t j = 0; j < channels; j++) {
			temp[i * channels + j] = data[j * samples + i];
		}
	}

	memcpy(data, temp, sizeof(temp));
}

static void usage(char *exe)
{
	printf("Usage: %s rec_file play_file out_file <\"aec args\">\n", basename(exe));
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	struct spa_dict_item items[16] = { 0, };
	int n_items = 0, res;

	if ((res = init(&data)) < 0)
		return res;

	if (argc < 4 || argc > 5) {
		usage(argv[0]);
		return -1;
	}

	if (argc == 5) {
		if ((res = spa_dict_from_json(items, SPA_N_ELEMENTS(items), argv[4])) < 0)
			return res;
		n_items = res;
	}

	struct spa_dict aec_args = SPA_DICT(items, n_items);

	struct spa_audio_info_raw rec_info = { 0, };
	struct spa_audio_info_raw play_info = { 0, };

	SNDFILE *rec_file = open_file_read(&data, argv[1], &rec_info);
	SNDFILE *play_file = open_file_read(&data, argv[2], &play_info);
	SNDFILE *out_file = open_file_write(&data, argv[3], &rec_info);

	if ((res = spa_audio_aec_init2(data.aec, &aec_args, &rec_info, &play_info, &rec_info)) < 0) {
		spa_log_error(data.log, "Could not initialise AEC engine: %s", spa_strerror(res));
		return -1;
	}

	if (data.aec->latency) {
		unsigned int num, denom;
		sscanf(data.aec->latency, "%u/%u", &num, &denom);
		data.aec_samples = rec_info.rate * num / denom;

	} else {
		/* Implementation doesn't care about the block size */
		data.aec_samples = 1024;
	}

	float rec_data[rec_info.channels * data.aec_samples];
	float play_data[play_info.channels * data.aec_samples];
	float out_data[rec_info.channels * data.aec_samples];

	const float *rec[rec_info.channels];
	const float *play[play_info.channels];
	float *out[rec_info.channels];

	for (uint32_t i = 0; i < rec_info.channels; i++) {
		rec[i] = &rec_data[i * data.aec_samples];
		out[i] = &out_data[i * data.aec_samples];
	}

	for (uint32_t i = 0; i < play_info.channels; i++) {
		play[i] = &play_data[i * data.aec_samples];
	}

	while (1) {
		res = sf_readf_float(rec_file, (float *)rec_data, data.aec_samples);
		if (res != (int) data.aec_samples)
			break;

		res = sf_readf_float(play_file, (float *)play_data, data.aec_samples);
		if (res != (int) data.aec_samples)
			break;

		deinterleave((float *)rec_data, rec_info.channels, data.aec_samples);
		deinterleave((float *)play_data, play_info.channels, data.aec_samples);

		spa_audio_aec_run(data.aec, rec, play, out, data.aec_samples);

		interleave((float *)out_data, rec_info.channels, data.aec_samples);

		res = sf_writef_float(out_file, (const float *)out_data, data.aec_samples);
		if (res != (int) data.aec_samples) {
			spa_log_error(data.log, "Failed to write: %s", spa_strerror(res));
			break;
		}
	}

	sf_close(rec_file);
	sf_close(play_file);
	sf_close(out_file);

	return 0;
}
