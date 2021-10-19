/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 *           © 2021 Arun Raghavan <arun@asymptotic.io>
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

#include "echo-cancel.h"

#include <pipewire/pipewire.h>

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/system_wrappers/include/trace.h>

struct impl {
	webrtc::AudioProcessing *apm = NULL;
	spa_audio_info_raw info;
	float** play_buffer;
	float** rec_buffer;
	float** out_buffer;
};

static void *webrtc_create(const struct pw_properties *args, const spa_audio_info_raw *info)
{
	struct impl *impl;
	webrtc::AudioProcessing *apm;
	webrtc::ProcessingConfig pconfig;
	webrtc::Config config;

	// TODO: wire up args to control these
	config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(true));
	// Enable Agnostic Delay Detection, would greatly improve performance for larger latencies
	config.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(true));

	apm = webrtc::AudioProcessing::Create(config);

	pconfig = {{
		webrtc::StreamConfig(info->rate, info->channels, false), /* input stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* output stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* reverse input stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* reverse output stream */
	}};

	if (apm->Initialize(pconfig) != webrtc::AudioProcessing::kNoError) {
		pw_log_error("Error initialising webrtc audio processing module");
		goto error;
	}

	// TODO: wire up args to control these
	apm->high_pass_filter()->Enable(true);
	apm->echo_cancellation()->enable_drift_compensation(false);
	apm->echo_cancellation()->Enable(true);
	apm->echo_cancellation()->set_suppression_level(webrtc::EchoCancellation::kHighSuppression);
        apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh);
	apm->noise_suppression()->Enable(true);
	// FIXME: wire up args to AGC
	// Note: AGC seems to mess up with Agnostic Delay Detection, especially with speech,
	// result in very poor performance, disable by default
	// apm->gain_control()->set_analog_level_limits(0, 255);
	// apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveDigital);
	// apm->gain_control()->Enable(true);

	impl = (struct impl *)calloc(1, sizeof(struct impl));
	impl->info = *info;

	impl->play_buffer = (float **)calloc(info->channels, sizeof(float*));
	impl->rec_buffer = (float **)calloc(info->channels, sizeof(float*));
	impl->out_buffer = (float **)calloc(info->channels, sizeof(float*));

	impl->apm = apm;

	return impl;

error:
	if (apm)
		delete apm;

	return NULL;
}

static void webrtc_destroy(void *ec)
{
	struct impl *impl = (struct impl*)ec;

	delete impl->apm;
	free(impl->play_buffer);
	free(impl->rec_buffer);
	free(impl->out_buffer);
	free(impl);
}

static int webrtc_run(void *ec, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	struct impl *impl = (struct impl*)ec;
	webrtc::StreamConfig config =
		webrtc::StreamConfig(impl->info.rate, impl->info.channels, false);
	unsigned int num_blocks = n_samples * 1000 / impl->info.rate / 10;

	if (n_samples * 1000 / impl->info.rate % 10 != 0) {
		pw_log_error("Buffers must be multiples of 10ms in length (currently %u samples)", n_samples);
		return -1;
	}

	for (size_t i = 0; i < num_blocks; i ++) {
		for (size_t j = 0; j < impl->info.channels; j++) {
			impl->play_buffer[j] = (float*)play[j] + config.num_frames() * i;
			impl->rec_buffer[j] = (float*)rec[j] + config.num_frames() * i;
			impl->out_buffer[j] = out[j] + config.num_frames() * i;
		}
		/* FIXME: ProcessReverseStream may change the playback buffer, in which
		* case we should use that, if we ever expose the intelligibility
		* enhancer */
		if (impl->apm->ProcessReverseStream(impl->play_buffer, config, config, impl->play_buffer) !=
				webrtc::AudioProcessing::kNoError) {
			pw_log_error("Processing reverse stream failed");
		}

		// Extra delay introduced by multiple frames
		impl->apm->set_stream_delay_ms((num_blocks - 1) * 10);

		if (impl->apm->ProcessStream(impl->rec_buffer, config, config, impl->out_buffer) !=
				webrtc::AudioProcessing::kNoError) {
			pw_log_error("Processing stream failed");
		}
	}

	return 0;
}

static const struct echo_cancel_info echo_cancel_webrtc_impl = {
	.name = "webrtc",
	.info = SPA_DICT_INIT(NULL, 0),
	.latency = "480/48000",

	.create = webrtc_create,
	.destroy = webrtc_destroy,

	.run = webrtc_run,
};

const struct echo_cancel_info *echo_cancel_webrtc = &echo_cancel_webrtc_impl;
