/* Spa libcamera support */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <memory>

#include <libcamera/camera_manager.h>

std::shared_ptr<libcamera::CameraManager> libcamera_manager_acquire(int& res);
