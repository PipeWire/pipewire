#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "compress-offload-api.h"


struct compress_offload_api_context {
	int fd;
	struct snd_compr_caps caps;
	struct spa_log *log;
	bool was_configured;
	uint32_t fragment_size;
	uint32_t num_fragments;
};


struct compress_offload_api_context* compress_offload_api_open(int card_nr, int device_nr, struct spa_log *log)
{
	struct compress_offload_api_context *context;
	char fn[256];

	assert(card_nr >= 0);
	assert(device_nr >= 0);
	assert(log != NULL);

	context = calloc(1, sizeof(struct compress_offload_api_context));
	if (context == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	context->log = log;

	snprintf(fn, sizeof(fn), "/dev/snd/comprC%uD%u", card_nr, device_nr);

	context->fd = open(fn, O_WRONLY);
	if (context->fd < 0) {
		spa_log_error(context->log, "could not open device \"%s\": %s (%d)", fn, strerror(errno), errno);
		goto error;
	}

	if (ioctl(context->fd, SNDRV_COMPRESS_GET_CAPS, &(context->caps)) != 0) {
		spa_log_error(context->log, "could not get device caps: %s (%d)", strerror(errno), errno);
		goto error;
	}

	return context;

error:
	compress_offload_api_close(context);
	if (errno == 0)
		errno = EIO;
	return NULL;
}


void compress_offload_api_close(struct compress_offload_api_context *context)
{
	if (context == NULL)
		return;

	if (context->fd > 0)
		close(context->fd);

	free(context);
}


int compress_offload_api_get_fd(struct compress_offload_api_context *context)
{
	assert(context != NULL);
	return context->fd;
}


int compress_offload_api_set_params(struct compress_offload_api_context *context, struct snd_codec *codec,
	                                uint32_t fragment_size, uint32_t num_fragments)
{
	struct snd_compr_params params;

	assert(context != NULL);
	assert(codec != NULL);
	assert(
		(fragment_size == 0) ||
		((fragment_size >= context->caps.min_fragment_size) && (fragment_size <= context->caps.max_fragment_size))
	);
	assert(
		(num_fragments == 0) ||
		((num_fragments >= context->caps.min_fragments) && (fragment_size <= context->caps.max_fragments))
	);

	context->fragment_size = (fragment_size != 0) ? fragment_size : context->caps.min_fragment_size;
	context->num_fragments = (num_fragments != 0) ? num_fragments : context->caps.max_fragments;

	memset(&params, 0, sizeof(params));
	params.buffer.fragment_size = context->fragment_size;
	params.buffer.fragments = context->num_fragments;
	memcpy(&(params.codec), codec, sizeof(struct snd_codec));

	if (ioctl(context->fd, SNDRV_COMPRESS_SET_PARAMS, &params) != 0) {
		spa_log_error(context->log, "could not set params: %s (%d)", strerror(errno), errno);
		return -errno;
	}

	context->was_configured = true;

	return 0;
}


void compress_offload_api_get_fragment_config(struct compress_offload_api_context *context,
                                              uint32_t *fragment_size, uint32_t *num_fragments)
{
	assert(context != NULL);
	assert(fragment_size != NULL);
	assert(num_fragments != NULL);

	*fragment_size = context->fragment_size;
	*num_fragments = context->num_fragments;
}


const struct snd_compr_caps * compress_offload_api_get_caps(struct compress_offload_api_context *context)
{
	assert(context != NULL);
	return &(context->caps);
}


int compress_offload_api_get_codec_caps(struct compress_offload_api_context *context,
                                        uint32_t codec_id, struct snd_compr_codec_caps *codec_caps)
{
	assert(context != NULL);
	assert(codec_id < SND_AUDIOCODEC_MAX);
	assert(codec_caps != NULL);

	memset(codec_caps, 0, sizeof(struct snd_compr_codec_caps));
	codec_caps->codec = codec_id;

	if (ioctl(context->fd, SNDRV_COMPRESS_GET_CODEC_CAPS, codec_caps) != 0) {
		spa_log_error(context->log, "could not get caps for codec with ID %#08x: %s (%d)",
		              codec_id, strerror(errno), errno);
		return -errno;
	}

	return 0;
}


bool compress_offload_api_supports_codec(struct compress_offload_api_context *context, uint32_t codec_id)
{
	uint32_t codec_index;

	assert(context != NULL);
	assert(codec_id < SND_AUDIOCODEC_MAX);

	for (codec_index = 0; codec_index < context->caps.num_codecs; ++codec_index) {
		if (context->caps.codecs[codec_index] == codec_id)
			return true;
	}

	return false;
}


#define RUN_SIMPLE_COMMAND(CONTEXT, CMD, CMD_NAME) \
{ \
	assert((CONTEXT) != NULL); \
	assert((CMD_NAME) != NULL); \
\
	if (ioctl((CONTEXT)->fd, (CMD)) < 0) { \
		spa_log_error((CONTEXT)->log, "could not %s device: %s (%d)", (CMD_NAME), strerror(errno), errno); \
		return -errno; \
	} \
\
	return 0; \
}


int compress_offload_api_start(struct compress_offload_api_context *context)
{
	RUN_SIMPLE_COMMAND(context, SNDRV_COMPRESS_START, "start");
}


int compress_offload_api_stop(struct compress_offload_api_context *context)
{
	RUN_SIMPLE_COMMAND(context, SNDRV_COMPRESS_STOP, "stop");
}


int compress_offload_api_pause(struct compress_offload_api_context *context)
{
	RUN_SIMPLE_COMMAND(context, SNDRV_COMPRESS_PAUSE, "pause");
}


int compress_offload_api_resume(struct compress_offload_api_context *context)
{
	RUN_SIMPLE_COMMAND(context, SNDRV_COMPRESS_RESUME, "resume");
}


int compress_offload_api_drain(struct compress_offload_api_context *context)
{
	RUN_SIMPLE_COMMAND(context, SNDRV_COMPRESS_DRAIN, "drain");
}


int compress_offload_api_get_timestamp(struct compress_offload_api_context *context,
                                       struct snd_compr_tstamp *timestamp)
{
	assert(context != NULL);
	assert(timestamp != NULL);

	if (ioctl(context->fd, SNDRV_COMPRESS_TSTAMP, timestamp) < 0) {
		spa_log_error(context->log, "could not get timestamp device: %s (%d)",
		              strerror(errno), errno);
		return -errno;
	}

	return 0;
}


int compress_offload_api_get_available_space(struct compress_offload_api_context *context,
                                             struct snd_compr_avail *available_space)
{
	assert(context != NULL);
	assert(available_space != NULL);

	if (ioctl(context->fd, SNDRV_COMPRESS_AVAIL, available_space) < 0) {
		spa_log_error(context->log, "could not get available space from device: %s (%d)",
		              strerror(errno), errno);
		return -errno;
	}

	return 0;
}


int compress_offload_api_write(struct compress_offload_api_context *context, const void *data, size_t size)
{
	int num_bytes_written;

	assert(context != NULL);
	assert(data != NULL);

	num_bytes_written = write(context->fd, data, size);
	if (num_bytes_written < 0) {
		switch (errno) {
			case EBADFD:
				/* EBADFD indicates that the device is paused and thus is not an error. */
				break;
			default:
				spa_log_error(context->log, "could not write %zu byte(s): %s (%d)",
				              size, strerror(errno), errno);
				break;
		}

		return -errno;
	}

	return num_bytes_written;
}
