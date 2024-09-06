/* Spa videoconvert plugin */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

extern const struct spa_handle_factory spa_videoadapter_factory;
extern const struct spa_handle_factory spa_videoconvert_dummy_factory;
#if HAVE_VIDEOCONVERT_FFMPEG
extern const struct spa_handle_factory spa_videoconvert_ffmpeg_factory;
#endif

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_videoadapter_factory;
		break;
	case 1:
		*factory = &spa_videoconvert_dummy_factory;
		break;
#if HAVE_VIDEOCONVERT_FFMPEG
	case 2:
		*factory = &spa_videoconvert_ffmpeg_factory;
		break;
#endif
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
