#ifndef SPA_FFMPEG_H
#define SPA_FFMPEG_H

#include <stdint.h>
#include <stddef.h>

struct spa_dict;
struct spa_handle;
struct spa_support;

int spa_ffmpeg_dec_init(struct spa_handle *handle, const struct spa_dict *info,
			const struct spa_support *support, uint32_t n_support);
int spa_ffmpeg_enc_init(struct spa_handle *handle, const struct spa_dict *info,
			const struct spa_support *support, uint32_t n_support);

#endif
