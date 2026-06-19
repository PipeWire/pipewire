/* Spa libcamera support */
/* SPDX-FileCopyrightText: Copyright © 2020 collabora */
/* SPDX-License-Identifier: MIT */

#include <memory>
#include <mutex>

#include <libcamera/camera_manager.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>

#include "libcamera.hpp"

extern "C" {

SPA_LOG_TOPIC_DEFINE(libcamera_log_topic, "spa.libcamera");

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory,
			uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_libcamera_manager_factory;
		break;
	case 1:
		*factory = &spa_libcamera_device_factory;
		break;
	case 2:
		*factory = &spa_libcamera_source_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

}

std::shared_ptr<libcamera::CameraManager> libcamera_manager_acquire(int& res)
{
	static std::weak_ptr<libcamera::CameraManager> global_manager;
	static std::mutex lock;

	std::lock_guard guard(lock);

	if (auto manager = global_manager.lock())
		return manager;

	auto manager = std::make_shared<libcamera::CameraManager>();
	if ((res = manager->start()) < 0)
		return {};

	global_manager = manager;

	return manager;
}
