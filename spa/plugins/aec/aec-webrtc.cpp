/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Arun Raghavan <arun@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <memory>
#include <utility>

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>
#include <spa/utils/json.h>
#include <spa/support/plugin.h>

#ifdef HAVE_WEBRTC
#include <webrtc/modules/audio_processing/include/audio_processing.h>
#include <webrtc/modules/interface/module_common_types.h>
#include <webrtc/system_wrappers/include/trace.h>
#else
#include <modules/audio_processing/include/audio_processing.h>
#endif

struct impl_data {
	struct spa_handle handle;
	struct spa_audio_aec aec;

	struct spa_log *log;
	std::unique_ptr<webrtc::AudioProcessing> apm;
	spa_audio_info_raw rec_info;
	spa_audio_info_raw out_info;
	spa_audio_info_raw play_info;
	std::unique_ptr<float *[]> play_buffer, rec_buffer, out_buffer;
};

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.eac.webrtc");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static bool webrtc_get_spa_bool(const struct spa_dict *args, const char *key, bool default_value)
{
	if (auto str = spa_dict_lookup(args, key))
		return spa_atob(str);

	return default_value;
}

#ifdef HAVE_WEBRTC
/* [ f0 f1 f2 ] */
static int parse_point(struct spa_json *it, float (&f)[3])
{
	struct spa_json arr;
	int i, res;

	if (spa_json_enter_array(it, &arr) <= 0)
		return -EINVAL;

	for (i = 0; i < 3; i++) {
		if ((res = spa_json_get_float(&arr, &f[i])) <= 0)
			return -EINVAL;
	}
	return 0;
}

/* [ point1 point2 ... ] */
static int parse_mic_geometry(struct impl_data *impl, const char *mic_geometry,
		std::vector<webrtc::Point>& geometry)
{
	int res;
	size_t i;
	struct spa_json it[2];

	spa_json_init(&it[0], mic_geometry, strlen(mic_geometry));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0) {
		spa_log_error(impl->log, "Error: webrtc.mic-geometry expects an array");
		return -EINVAL;
	}

	for (i = 0; i < geometry.size(); i++) {
		float f[3];

		if ((res = parse_point(&it[1], f)) < 0) {
			spa_log_error(impl->log, "Error: can't parse webrtc.mic-geometry points: %d", res);
			return res;
		}

		spa_log_info(impl->log, "mic %zd position: (%g %g %g)", i, f[0], f[1], f[2]);
		geometry[i].c[0] = f[0];
		geometry[i].c[1] = f[1];
		geometry[i].c[2] = f[2];
	}
	return 0;
}
#endif

static int webrtc_init2(void *object, const struct spa_dict *args,
		struct spa_audio_info_raw *rec_info, struct spa_audio_info_raw *out_info,
		struct spa_audio_info_raw *play_info)
{
	auto impl = static_cast<struct impl_data*>(object);
	int res;

	bool high_pass_filter = webrtc_get_spa_bool(args, "webrtc.high_pass_filter", true);
	bool noise_suppression = webrtc_get_spa_bool(args, "webrtc.noise_suppression", true);
	bool voice_detection = webrtc_get_spa_bool(args, "webrtc.voice_detection", true);
#ifdef HAVE_WEBRTC
	bool extended_filter = webrtc_get_spa_bool(args, "webrtc.extended_filter", true);
	bool delay_agnostic = webrtc_get_spa_bool(args, "webrtc.delay_agnostic", true);
	// Disable experimental flags by default
	bool experimental_agc = webrtc_get_spa_bool(args, "webrtc.experimental_agc", false);
	bool experimental_ns = webrtc_get_spa_bool(args, "webrtc.experimental_ns", false);

	bool beamforming = webrtc_get_spa_bool(args, "webrtc.beamforming", false);
#else
	bool transient_suppression = webrtc_get_spa_bool(args, "webrtc.transient_suppression", true);
#endif
	// Note: AGC seems to mess up with Agnostic Delay Detection, especially with speech,
	// result in very poor performance, disable by default
	bool gain_control = webrtc_get_spa_bool(args, "webrtc.gain_control", false);

	// FIXME: Intelligibility enhancer is not currently supported
	// This filter will modify playback buffer (when calling ProcessReverseStream), but now
	// playback buffer modifications are discarded.

#ifdef HAVE_WEBRTC
	webrtc::Config config;
	config.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(extended_filter));
	config.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(delay_agnostic));
	config.Set<webrtc::ExperimentalAgc>(new webrtc::ExperimentalAgc(experimental_agc));
	config.Set<webrtc::ExperimentalNs>(new webrtc::ExperimentalNs(experimental_ns));

	if (beamforming) {
		std::vector<webrtc::Point> geometry(rec_info->channels);
		const char *mic_geometry, *target_direction;

		/* The beamformer gives a single mono channel */
		out_info->channels = 1;
		out_info->position[0] = SPA_AUDIO_CHANNEL_MONO;

		if ((mic_geometry = spa_dict_lookup(args, "webrtc.mic-geometry")) == NULL) {
			spa_log_error(impl->log, "Error: webrtc.beamforming requires webrtc.mic-geometry");
			return -EINVAL;
		}

		if ((res = parse_mic_geometry(impl, mic_geometry, geometry)) < 0)
			return res;

		if ((target_direction = spa_dict_lookup(args, "webrtc.target-direction")) != NULL) {
			webrtc::SphericalPointf direction(0.0f, 0.0f, 0.0f);
			struct spa_json it;
			float f[3];

			spa_json_init(&it, target_direction, strlen(target_direction));
			if (parse_point(&it, f) < 0) {
				spa_log_error(impl->log, "Error: can't parse target-direction %s",
						target_direction);
				return -EINVAL;
			}

			direction.s[0] = f[0];
			direction.s[1] = f[1];
			direction.s[2] = f[2];

			config.Set<webrtc::Beamforming>(new webrtc::Beamforming(true, geometry, direction));
		} else {
			config.Set<webrtc::Beamforming>(new webrtc::Beamforming(true, geometry));
		}
	}
#else
	webrtc::AudioProcessing::Config config;
	config.echo_canceller.enabled = true;
	// FIXME: Example code enables both gain controllers, but that seems sus
	config.gain_controller1.enabled = gain_control;
	config.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::Mode::kAdaptiveDigital;
	config.gain_controller1.analog_level_minimum = 0;
	config.gain_controller1.analog_level_maximum = 255;
	config.gain_controller2.enabled = gain_control;
	config.high_pass_filter.enabled = high_pass_filter;
	config.noise_suppression.enabled = noise_suppression;
	config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
	// FIXME: expose pre/postamp gain
	config.transient_suppression.enabled = transient_suppression;
	config.voice_detection.enabled = voice_detection;
#endif

	webrtc::ProcessingConfig pconfig = {{
		webrtc::StreamConfig(rec_info->rate, rec_info->channels, false), /* input stream */
		webrtc::StreamConfig(out_info->rate, out_info->channels, false), /* output stream */
		webrtc::StreamConfig(play_info->rate, play_info->channels, false), /* reverse input stream */
		webrtc::StreamConfig(play_info->rate, play_info->channels, false), /* reverse output stream */
	}};

#ifdef HAVE_WEBRTC
	auto apm = std::unique_ptr<webrtc::AudioProcessing>(webrtc::AudioProcessing::Create(config));
#else
	auto apm = std::unique_ptr<webrtc::AudioProcessing>(webrtc::AudioProcessingBuilder().Create());

	apm->ApplyConfig(config);
#endif

	if ((res = apm->Initialize(pconfig)) != webrtc::AudioProcessing::kNoError) {
		spa_log_error(impl->log, "Error initialising webrtc audio processing module: %d", res);
		return -EINVAL;
	}

#ifdef HAVE_WEBRTC
	apm->high_pass_filter()->Enable(high_pass_filter);
	// Always disable drift compensation since PipeWire will already do
	// drift compensation on all sinks and sources linked to this echo-canceler
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
#endif
	impl->apm = std::move(apm);
	impl->rec_info = *rec_info;
	impl->out_info = *out_info;
	impl->play_info = *play_info;
	impl->play_buffer = std::make_unique<float *[]>(play_info->channels);
	impl->rec_buffer = std::make_unique<float *[]>(rec_info->channels);
	impl->out_buffer = std::make_unique<float *[]>(out_info->channels);
	return 0;
}

static int webrtc_init(void *object, const struct spa_dict *args,
		const struct spa_audio_info_raw *info)
{
	int res;
	struct spa_audio_info_raw rec_info = *info;
	struct spa_audio_info_raw out_info = *info;
	struct spa_audio_info_raw play_info = *info;
	res = webrtc_init2(object, args, &rec_info, &out_info, &play_info);
	if (rec_info.channels != out_info.channels)
		res = -EINVAL;
	return res;
}

static int webrtc_run(void *object, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	auto impl = static_cast<struct impl_data*>(object);
	int res;

	webrtc::StreamConfig play_config =
		webrtc::StreamConfig(impl->play_info.rate, impl->play_info.channels, false);
	webrtc::StreamConfig rec_config =
		webrtc::StreamConfig(impl->rec_info.rate, impl->rec_info.channels, false);
	webrtc::StreamConfig out_config =
		webrtc::StreamConfig(impl->out_info.rate, impl->out_info.channels, false);
	unsigned int num_blocks = n_samples * 1000 / impl->play_info.rate / 10;

	if (n_samples * 1000 / impl->play_info.rate % 10 != 0) {
		spa_log_error(impl->log, "Buffers must be multiples of 10ms in length (currently %u samples)", n_samples);
		return -EINVAL;
	}

	for (size_t i = 0; i < num_blocks; i ++) {
		for (size_t j = 0; j < impl->play_info.channels; j++)
			impl->play_buffer[j] = const_cast<float *>(play[j]) + play_config.num_frames() * i;
		for (size_t j = 0; j < impl->rec_info.channels; j++)
			impl->rec_buffer[j] = const_cast<float *>(rec[j]) + rec_config.num_frames() * i;
		for (size_t j = 0; j < impl->out_info.channels; j++)
			impl->out_buffer[j] = out[j] + out_config.num_frames() * i;

		/* FIXME: ProcessReverseStream may change the playback buffer, in which
		* case we should use that, if we ever expose the intelligibility
		* enhancer */
		if ((res = impl->apm->ProcessReverseStream(impl->play_buffer.get(),
					play_config, play_config, impl->play_buffer.get())) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing reverse stream failed: %d", res);
		}

		// Extra delay introduced by multiple frames
		impl->apm->set_stream_delay_ms((num_blocks - 1) * 10);

		if ((res = impl->apm->ProcessStream(impl->rec_buffer.get(),
					rec_config, out_config, impl->out_buffer.get())) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing stream failed: %d", res);
		}
	}
	return 0;
}

static const struct spa_audio_aec_methods impl_aec = {
	SPA_VERSION_AUDIO_AEC_METHODS,
	.add_listener = NULL,
	.init = webrtc_init,
	.run = webrtc_run,
	.init2 = webrtc_init2,
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

	impl->log = static_cast<struct spa_log *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log));
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

static const struct spa_handle_factory spa_aec_webrtc_factory = {
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
		*factory = &spa_aec_webrtc_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
