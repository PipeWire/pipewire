#include <errno.h>
#include <inttypes.h>
#include "compress-offload-api.h"
#include "compress-offload-api-util.h"

int get_compress_offload_device_direction(int card_nr, int device_nr,
                                          struct spa_log *log,
                                          enum spa_compress_offload_direction *direction)
{
	int ret = 0;
	struct compress_offload_api_context *device_context;
	const struct snd_compr_caps *compr_caps;

	device_context = compress_offload_api_open(card_nr, device_nr, log);
	if (device_context == NULL)
		return -errno;

	compr_caps = compress_offload_api_get_caps(device_context);

	switch (compr_caps->direction) {
	case SND_COMPRESS_PLAYBACK:
		*direction = SPA_COMPRESS_OFFLOAD_DIRECTION_PLAYBACK;
		break;
	case SND_COMPRESS_CAPTURE:
		*direction = SPA_COMPRESS_OFFLOAD_DIRECTION_CAPTURE;
		break;
	default:
		spa_log_error(log, "card nr %d device nr %d: unknown direction %#" PRIx32,
		             card_nr, device_nr, (uint32_t)(compr_caps->direction));
		ret = -EINVAL;
	}

	compress_offload_api_close(device_context);

	return ret;
}
