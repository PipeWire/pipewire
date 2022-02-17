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

#include <memory>
#include <utility>

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>

#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/system_wrappers/include/trace.h>

struct impl_data {
	struct spa_handle handle;
	struct spa_audio_aec aec;

	struct spa_log *log;
	std::unique_ptr<webrtc::AudioProcessing> apm;
	spa_audio_info_raw info;
	std::unique_ptr<float *[]> play_buffer, rec_buffer, out_buffer;
};

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.eac.webrtc");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static bool webrtc_get_spa_bool(const struct spa_dict *args, const char *key, bool default_value) {
	const char *str_val;
	bool value = default_value;
	str_val = spa_dict_lookup(args, key);
	if (str_val != NULL)
		value =spa_atob(str_val);

	return value;
}

static int webrtc_init(void *data, const struct spa_dict *args, const struct spa_audio_info_raw *info)
{
	auto impl = reinterpret_cast<struct impl_data*>(data);

	bool extended_filter = webrtc_get_spa_bool(args, "webrtc.extended_filter", true);
	bool delay_agnostic = webrtc_get_spa_bool(args, "webrtc.delay_agnostic", true);
	bool high_pass_filter = webrtc_get_spa_bool(args, "webrtc.high_pass_filter", true);
	bool noise_suppression = webrtc_get_spa_bool(args, "webrtc.noise_suppression", true);
	bool voice_detection = webrtc_get_spa_bool(args, "webrtc.voice_detection", true);

	// Note: AGC seems to mess up with Agnostic Delay Detection, especially with speech,
	// result in very poor performance, disable by default
	bool gain_control = webrtc_get_spa_bool(args, "webrtc.gain_control", false);

	// Disable experimental flags by default
	bool experimental_agc = webrtc_get_spa_bool(args, "webrtc.experimental_agc", false);
	bool experimental_ns = webrtc_get_spa_bool(args, "webrtc.experimental_ns", false);

	// FIXME: Intelligibility enhancer is not currently supported
	// This filter will modify playback buffer (when calling ProcessReverseStream), but now
	// playback buffer modifications are discarded.

	webrtc::Config config;
	config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(extended_filter));
	config.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(delay_agnostic));
	config.Set<webrtc::ExperimentalAgc>(new webrtc::ExperimentalAgc(experimental_agc));
	config.Set<webrtc::ExperimentalNs>(new webrtc::ExperimentalNs(experimental_ns));

	webrtc::ProcessingConfig pconfig = {{
		webrtc::StreamConfig(info->rate, info->channels, false), /* input stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* output stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* reverse input stream */
		webrtc::StreamConfig(info->rate, info->channels, false), /* reverse output stream */
	}};

	auto apm = std::unique_ptr<webrtc::AudioProcessing>(webrtc::AudioProcessing::Create(config));
	if (apm->Initialize(pconfig) != webrtc::AudioProcessing::kNoError) {
		spa_log_error(impl->log, "Error initialising webrtc audio processing module");
		return -1;
	}

	apm->high_pass_filter()->Enable(high_pass_filter);
	// Always disable drift compensation since it requires drift sampling
	apm->echo_cancellation()->enable_drift_compensation(false);
	apm->echo_cancellation()->Enable(true);
	// TODO: wire up supression levels to args
	apm->echo_cancellation()->set_suppression_level(webrtc::EchoCancellation::kHighSuppression);
	apm->noise_suppression()->set_level(webrtc::NoiseSuppression::kHigh);
	apm->noise_suppression()->Enable(noise_suppression);
	apm->voice_detection()->Enable(voice_detection);
	// TODO: wire up AGC parameters to args
	apm->gain_control()->set_analog_level_limits(0, 255);
	apm->gain_control()->set_mode(webrtc::GainControl::kAdaptiveDigital);
	apm->gain_control()->Enable(gain_control);
	impl->apm = std::move(apm);
	impl->info = *info;
	impl->play_buffer = std::make_unique<float *[]>(info->channels);
	impl->rec_buffer = std::make_unique<float *[]>(info->channels);
	impl->out_buffer = std::make_unique<float *[]>(info->channels);
	return 0;
}

static int webrtc_run(void *data, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	auto impl = reinterpret_cast<struct impl_data*>(data);
	webrtc::StreamConfig config =
		webrtc::StreamConfig(impl->info.rate, impl->info.channels, false);
	unsigned int num_blocks = n_samples * 1000 / impl->info.rate / 10;

	if (n_samples * 1000 / impl->info.rate % 10 != 0) {
		spa_log_error(impl->log, "Buffers must be multiples of 10ms in length (currently %u samples)", n_samples);
		return -1;
	}

	for (size_t i = 0; i < num_blocks; i ++) {
		for (size_t j = 0; j < impl->info.channels; j++) {
			impl->play_buffer[j] = const_cast<float *>(play[j]) + config.num_frames() * i;
			impl->rec_buffer[j] = const_cast<float *>(rec[j]) + config.num_frames() * i;
			impl->out_buffer[j] = out[j] + config.num_frames() * i;
		}
		/* FIXME: ProcessReverseStream may change the playback buffer, in which
		* case we should use that, if we ever expose the intelligibility
		* enhancer */
		if (impl->apm->ProcessReverseStream(impl->play_buffer.get(), config, config, impl->play_buffer.get()) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing reverse stream failed");
		}

		// Extra delay introduced by multiple frames
		impl->apm->set_stream_delay_ms((num_blocks - 1) * 10);

		if (impl->apm->ProcessStream(impl->rec_buffer.get(), config, config, impl->out_buffer.get()) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing stream failed");
		}
	}

	return 0;
}

static struct spa_audio_aec_methods impl_aec = {
	SPA_VERSION_AUDIO_AEC_METHODS,
	.add_listener = NULL,
	.init = webrtc_init,
	.run = webrtc_run,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto impl = reinterpret_cast<struct impl_data*>(handle);

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_AUDIO_AEC))
		*interface = &impl->aec;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	auto impl = reinterpret_cast<struct impl_data*>(handle);
	impl->~impl_data();
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl_data);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	auto impl = new (handle) impl_data();

	impl->handle.get_interface = impl_get_interface;
	impl->handle.clear = impl_clear;

	impl->aec.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_AUDIO_AEC,
		SPA_VERSION_AUDIO_AEC,
		&impl_aec, impl);
	impl->aec.name = "webrtc",
	impl->aec.info = NULL;
	impl->aec.latency = "480/48000",

	impl->log = (struct spa_log*)spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_AUDIO_AEC,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_aec_exaudio_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AEC,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};


SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_aec_exaudio_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
