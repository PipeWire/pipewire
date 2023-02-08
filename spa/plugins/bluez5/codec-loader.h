/* Spa A2DP codec API */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_CODEC_LOADER_H_
#define SPA_BLUEZ5_CODEC_LOADER_H_

#include <stdint.h>
#include <stddef.h>

#include <spa/support/plugin-loader.h>

#include "a2dp-codec-caps.h"
#include "media-codecs.h"

const struct media_codec * const *load_media_codecs(struct spa_plugin_loader *loader, struct spa_log *log);
void free_media_codecs(const struct media_codec * const *media_codecs);

#endif
