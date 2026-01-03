/* Spa libcamera support */
/* SPDX-FileCopyrightText: Copyright © 2020 collabora */
/* SPDX-License-Identifier: MIT */

#pragma once

#include <memory>

#include <libcamera/camera_manager.h>
#include <libcamera/version.h>

#include <spa/support/log.h>
#include <spa/utils/defs.h>

#define KEY_VERSION_LIBRARY	"api.libcamera.version.library"
#define KEY_VERSION_HEADER	"api.libcamera.version.header"

extern "C" {

extern const struct spa_handle_factory spa_libcamera_source_factory;
extern const struct spa_handle_factory spa_libcamera_manager_factory;
extern const struct spa_handle_factory spa_libcamera_device_factory;

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &libcamera_log_topic
extern struct spa_log_topic libcamera_log_topic;

}

static inline void libcamera_log_topic_init(struct spa_log *log)
{
	spa_log_topic_init(log, &libcamera_log_topic);
}

static inline const char *libcamera_library_version()
{
	return libcamera::CameraManager::version().c_str();
}

static inline const char *libcamera_header_version()
{
	return
		SPA_STRINGIFY(LIBCAMERA_VERSION_MAJOR) "."
		SPA_STRINGIFY(LIBCAMERA_VERSION_MINOR) "."
		SPA_STRINGIFY(LIBCAMERA_VERSION_PATCH);
}

std::shared_ptr<libcamera::CameraManager> libcamera_manager_acquire(int& res);
