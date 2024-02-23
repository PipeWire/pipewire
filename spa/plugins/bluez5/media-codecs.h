/* Spa A2DP codec API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_A2DP_CODECS_H_
#define SPA_BLUEZ5_A2DP_CODECS_H_

#include <stdint.h>
#include <stddef.h>

#include <spa/param/audio/format.h>
#include <spa/param/bluetooth/audio.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/support/log.h>

#include "a2dp-codec-caps.h"
#include "bap-codec-caps.h"

/*
 * The codec plugin SPA interface is private.  The version should be incremented
 * when any of the structs or semantics change.
 */

#define SPA_TYPE_INTERFACE_Bluez5CodecMedia	SPA_TYPE_INFO_INTERFACE_BASE "Bluez5:Codec:Media:Private"

#define SPA_VERSION_BLUEZ5_CODEC_MEDIA		10

struct spa_bluez5_codec_a2dp {
	struct spa_interface iface;
	const struct media_codec * const *codecs;	/**< NULL terminated array */
};

#define MEDIA_CODEC_FACTORY_NAME(basename)		(SPA_NAME_API_CODEC_BLUEZ5_MEDIA "." basename)

#ifdef CODEC_PLUGIN
#define MEDIA_CODEC_EXPORT_DEF(basename,...)	\
	const char *codec_plugin_factory_name = MEDIA_CODEC_FACTORY_NAME(basename); \
	static const struct media_codec * const codec_plugin_media_codec_list[] = { __VA_ARGS__, NULL };	\
	const struct media_codec * const * const codec_plugin_media_codecs = codec_plugin_media_codec_list;	\
	SPA_LOG_TOPIC_DEFINE(codec_plugin_log_topic, "spa.bluez5.codecs." basename);

extern const struct media_codec * const * const codec_plugin_media_codecs;
extern const char *codec_plugin_factory_name;
extern struct spa_log_topic codec_plugin_log_topic;
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &codec_plugin_log_topic
#endif

#define MEDIA_CODEC_FLAG_SINK		(1 << 0)

#define A2DP_CODEC_DEFAULT_RATE		48000
#define A2DP_CODEC_DEFAULT_CHANNELS	2

enum {
	NEED_FLUSH_NO = 0,
	NEED_FLUSH_ALL = 1,
	NEED_FLUSH_FRAGMENT = 2,
};

struct media_codec_audio_info {
	uint32_t rate;
	uint32_t channels;
};

struct media_codec {
	enum spa_bluetooth_audio_codec id;
	uint8_t codec_id;
	a2dp_vendor_codec_t vendor;

	bool bap;

	const char *name;
	const char *description;
	const char *endpoint_name;	/**< Endpoint name. If NULL, same as name */
	const struct spa_dict *info;

	const size_t send_buf_size;

	const struct media_codec *duplex_codec;	/**< Codec for non-standard A2DP duplex channel */

	int (*get_bis_config)(const struct media_codec *codec, uint8_t *caps,
				uint8_t *caps_size,	struct spa_dict *settings,
				struct bap_codec_qos *qos);

	/** If fill_caps is NULL, no endpoint is registered (for sharing with another codec). */
	int (*fill_caps) (const struct media_codec *codec, uint32_t flags,
			uint8_t caps[A2DP_MAX_CAPS_SIZE]);

	int (*select_config) (const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			const struct media_codec_audio_info *info,
			const struct spa_dict *global_settings, uint8_t config[A2DP_MAX_CAPS_SIZE]);
	int (*enum_config) (const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
			struct spa_pod_builder *builder, struct spa_pod **param);
	int (*validate_config) (const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info);
	int (*get_qos)(const struct media_codec *codec,
			const void *config, size_t config_size,
			const struct bap_endpoint_qos *endpoint_qos,
			struct bap_codec_qos *qos);

	/** qsort comparison sorting caps in order of preference for the codec.
	 * Used in codec switching to select best remote endpoints.
	 * The caps handed in correspond to this codec_id, but are
	 * otherwise not checked beforehand.
	 */
	int (*caps_preference_cmp) (const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
			const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info,
			const struct spa_dict *global_settings);

	void *(*init_props) (const struct media_codec *codec, uint32_t flags, const struct spa_dict *settings);
	void (*clear_props) (void *);
	int (*enum_props) (void *props, const struct spa_dict *settings, uint32_t id, uint32_t idx,
			struct spa_pod_builder *builder, struct spa_pod **param);
	int (*set_props) (void *props, const struct spa_pod *param);

	void *(*init) (const struct media_codec *codec, uint32_t flags, void *config, size_t config_size,
			const struct spa_audio_info *info, void *props, size_t mtu);
	void (*deinit) (void *data);

	int (*update_props) (void *data, void *props);

	/** Number of bytes needed for encoding */
	int (*get_block_size) (void *data);

	/**
	 * Duration of the next packet in nanoseconds.
	 *
	 * For BAP this shall be constant and equal to the SDU interval.
	 *
	 * \param data Codec data from init()
	 * \return Duration in nanoseconds.
	 */
	uint64_t (*get_interval) (void *data);

	int (*abr_process) (void *data, size_t unsent);

	/**
	 * Start encoding new packet.
	 *
	 * \param data Codec data from init()
	 * \param timestamp Packet time stamp (in samples played)
	 * \return Size of packet header written to dst in bytes, or < 0 for error
	 */
	int (*start_encode) (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp);

	/**
	 * Consume data from input buffer, encode to output buffer.
	 *
	 * \param data Codec data from init()
	 * \param src Source data. NULL if encoding packet fragment.
	 * \param dst Output buffer position. The memory region passed to the
	 *    previous start_encode() is still valid, and this position is inside
	 *    that region. The caller does not modify the contents of the buffer.
	 * \param dst_size Remaining buffer space after dst
	 * \param dst_out Bytes written to dst
	 * \param need_flush
	 *    - NEED_FLUSH_NO: don't flush this packet,
	 *    - NEED_FLUSH_ALL: flush this packet,
	 *    - NEED_FLUSH_FRAGMENT: flush packet fragment. The next start_encode()
	 *      and encode() are expected to produce more fragments or the final
	 *      fragment with NEED_FLUSH_ALL, without consuming source data.
	 *      The fragment start_encode() is called with the same output buffer
	 *      as previous. The fragment encode() will be called with NULL src.
	 *      No new source data will be fed in before NEED_FLUSH_ALL.
	 * \return Number of bytes consumed from src, or < 0 for error
	 */
	int (*encode) (void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush);

	/**
	 * Start decoding received packet.
	 *
	 * \return Number of bytes consumed from source data, or < 0 for error
	 */
	int (*start_decode) (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp);

	/**
	 * Decode received packet data.
	 *
	 * \param dst_out Number of bytes output to dst
	 * \return Number of bytes consumed from src, or < 0 for error
	 */
	int (*decode) (void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out);

	int (*reduce_bitpool) (void *data);
	int (*increase_bitpool) (void *data);

	void (*set_log) (struct spa_log *global_log);

	/**
	 * Get codec internal delays, in samples at input/output rates.
	 *
	 * The delay does not include the duration of the PCM input/output
	 * audio data, but is that internal to the codec.
	 *
	 * \param[out] encoder    Encoder delay in samples, or NULL
	 * \param[out] decoder    Decoder delay in samples, or NULL
	 */
	void (*get_delay) (void *data, uint32_t *encoder, uint32_t *decoder);
};

struct media_codec_config {
	uint32_t config;
	int value;
	unsigned int priority;
};

int media_codec_select_config(const struct media_codec_config configs[], size_t n,
	uint32_t cap, int preferred_value);

int media_codec_get_config(const struct media_codec_config configs[], size_t n,
	uint32_t conf);

bool media_codec_check_caps(const struct media_codec *codec, unsigned int codec_id,
	const void *caps, size_t caps_size, const struct media_codec_audio_info *info,
	const struct spa_dict *global_settings);

#endif
