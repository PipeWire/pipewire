#ifndef COMPRESS_OFFLOAD_API_H
#define COMPRESS_OFFLOAD_API_H

#include <stdint.h>
#include <stdbool.h>
#include <sound/compress_offload.h>
#include <sound/compress_params.h>
#include <spa/support/log.h>


struct compress_offload_api_context;


#if defined(__GNUC__) && __GNUC__ >= 4
#define COMPR_API_PRIVATE __attribute__((visibility("hidden")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define COMPR_API_PRIVATE __attribute__((visibility("hidden")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x550)
#define COMPR_API_PRIVATE __hidden
#else
#define COMPR_API_PRIVATE
#endif


/* This is a simple encapsulation of the ALSA Compress-Offload API
 * and its ioctl calls. It is intentionally not using any PipeWire
 * or SPA headers to allow for porting it or extracting it as its
 * own library in the future if needed. It functions as an alternative
 * to tinycompress, and was written, because tinycompress lacks
 * critical functionality (it does not expose important device caps)
 * and adds little value in this particular use case.
 *
 * Encapsulating the ioctls behind this API also allows for using
 * different backends. This might be interesting in the future for
 * testing purposes; for example, an alternative backend could exist
 * that emulates a compress-offload device by decoding with FFmpeg.
 * This would be useful for debugging compressed audio related issues
 * in PipeWire on the PC - an important advantage, since getting to
 * actual compress-offload hardware can sometimes be difficult. */


COMPR_API_PRIVATE struct compress_offload_api_context* compress_offload_api_open(int card_nr, int device_nr,
	                                                                             struct spa_log *log);
COMPR_API_PRIVATE void compress_offload_api_close(struct compress_offload_api_context *context);

COMPR_API_PRIVATE int compress_offload_api_get_fd(struct compress_offload_api_context *context);

COMPR_API_PRIVATE int compress_offload_api_set_params(struct compress_offload_api_context *context,
	                                                  struct snd_codec *codec, uint32_t fragment_size,
	                                                  uint32_t num_fragments);
COMPR_API_PRIVATE void compress_offload_api_get_fragment_config(struct compress_offload_api_context *context,
                                                                uint32_t *fragment_size, uint32_t *num_fragments);

COMPR_API_PRIVATE const struct snd_compr_caps * compress_offload_api_get_caps(struct compress_offload_api_context *context);
COMPR_API_PRIVATE int compress_offload_api_get_codec_caps(struct compress_offload_api_context *context,
	                                                      uint32_t codec_id, struct snd_compr_codec_caps *codec_caps);
COMPR_API_PRIVATE bool compress_offload_api_supports_codec(struct compress_offload_api_context *context, uint32_t codec_id);

COMPR_API_PRIVATE int compress_offload_api_start(struct compress_offload_api_context *context);
COMPR_API_PRIVATE int compress_offload_api_stop(struct compress_offload_api_context *context);

COMPR_API_PRIVATE int compress_offload_api_pause(struct compress_offload_api_context *context);
COMPR_API_PRIVATE int compress_offload_api_resume(struct compress_offload_api_context *context);

COMPR_API_PRIVATE int compress_offload_api_drain(struct compress_offload_api_context *context);

COMPR_API_PRIVATE int compress_offload_api_get_timestamp(struct compress_offload_api_context *context,
	                                                           struct snd_compr_tstamp *timestamp);
COMPR_API_PRIVATE int compress_offload_api_get_available_space(struct compress_offload_api_context *context,
	                                                           struct snd_compr_avail *available_space);

COMPR_API_PRIVATE int compress_offload_api_write(struct compress_offload_api_context *context,
	                                             const void *data, size_t size);


#endif /* COMPRESS_OFFLOAD_API_H */
