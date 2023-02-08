/* Spa ALSA support */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

extern const struct spa_handle_factory spa_alsa_source_factory;
extern const struct spa_handle_factory spa_alsa_sink_factory;
extern const struct spa_handle_factory spa_alsa_udev_factory;
extern const struct spa_handle_factory spa_alsa_device_factory;
extern const struct spa_handle_factory spa_alsa_seq_bridge_factory;
extern const struct spa_handle_factory spa_alsa_acp_device_factory;
#ifdef HAVE_ALSA_COMPRESS_OFFLOAD
extern const struct spa_handle_factory spa_alsa_compress_offload_sink_factory;
#endif

struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.alsa");
struct spa_log_topic *alsa_log_topic = &log_topic;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_alsa_source_factory;
		break;
	case 1:
		*factory = &spa_alsa_sink_factory;
		break;
	case 2:
		*factory = &spa_alsa_udev_factory;
		break;
	case 3:
		*factory = &spa_alsa_device_factory;
		break;
	case 4:
		*factory = &spa_alsa_seq_bridge_factory;
		break;
	case 5:
		*factory = &spa_alsa_acp_device_factory;
		break;
#ifdef HAVE_ALSA_COMPRESS_OFFLOAD
	case 6:
		*factory = &spa_alsa_compress_offload_sink_factory;
		break;
#endif
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
