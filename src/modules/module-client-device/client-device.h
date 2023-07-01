/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_CLIENT_DEVICE_H
#define PIPEWIRE_CLIENT_DEVICE_H

#include <pipewire/impl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLIENT_DEVICE_USAGE	"["PW_KEY_DEVICE_NAME"=<string>]"

struct pw_impl_device *
pw_client_device_new(struct pw_resource *resource,
		   struct pw_properties *properties);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CLIENT_DEVICE_H */
