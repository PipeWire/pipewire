/* Spa libcamera support */
/* SPDX-FileCopyrightText: Copyright © 2020 collabora */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <linux/media.h>

#include <spa/support/log.h>
#include <spa/support/system.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern const struct spa_handle_factory spa_libcamera_source_factory;
extern const struct spa_handle_factory spa_libcamera_manager_factory;
extern const struct spa_handle_factory spa_libcamera_device_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &libcamera_log_topic
extern struct spa_log_topic libcamera_log_topic;

static inline void libcamera_log_topic_init(struct spa_log *log)
{
	spa_log_topic_init(log, &libcamera_log_topic);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
