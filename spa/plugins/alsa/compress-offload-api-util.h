#ifndef SPA_ALSA_COMPRESS_OFFLOAD_DEVICE_UTIL_H
#define SPA_ALSA_COMPRESS_OFFLOAD_DEVICE_UTIL_H

#include <spa/support/log.h>

#if defined(__GNUC__) && __GNUC__ >= 4
#define COMPR_API_PRIVATE __attribute__((visibility("hidden")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590)
#define COMPR_API_PRIVATE __attribute__((visibility("hidden")))
#elif defined(__SUNPRO_C) && (__SUNPRO_C >= 0x550)
#define COMPR_API_PRIVATE __hidden
#else
#define COMPR_API_PRIVATE
#endif

enum spa_compress_offload_direction {
	SPA_COMPRESS_OFFLOAD_DIRECTION_PLAYBACK,
	SPA_COMPRESS_OFFLOAD_DIRECTION_CAPTURE
};

/* This exists for situations where both the direction of the compress-offload
 * device and the functions from asoundlib.h are needed. It is not possible to
 * include asoundlib.h and the compress-offload headers in the same C file,
 * since these headers contain conflicting declarations. Provide this direction
 * check function to keep the compress-offload headers encapsulated. */
COMPR_API_PRIVATE int get_compress_offload_device_direction(int card_nr, int device_nr,
                                                            struct spa_log *log,
                                                            enum spa_compress_offload_direction *direction);

#endif /* SPA_ALSA_COMPRESS_OFFLOAD_DEVICE_UTIL_H */
