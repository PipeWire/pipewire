/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_SPA_DEVICE_H
#define PIPEWIRE_SPA_DEVICE_H

#include <spa/monitor/device.h>

#include <pipewire/impl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pw_spa_device_flags {
	PW_SPA_DEVICE_FLAG_DISABLE	= (1 << 0),
	PW_SPA_DEVICE_FLAG_NO_REGISTER	= (1 << 1),
};

struct pw_impl_device *
pw_spa_device_new(struct pw_context *context,
		  enum pw_spa_device_flags flags,
		  struct spa_device *device,
		  struct spa_handle *handle,
		  struct pw_properties *properties,
		  size_t user_data_size);

struct pw_impl_device *
pw_spa_device_load(struct pw_context *context,
		   const char *factory_name,
		   enum pw_spa_device_flags flags,
		   struct pw_properties *properties,
		   size_t user_data_size);

void *pw_spa_device_get_user_data(struct pw_impl_device *device);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_SPA_DEVICE_H */
