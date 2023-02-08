/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_ADAPTER_H
#define PIPEWIRE_ADAPTER_H

#include <pipewire/impl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADAPTER_USAGE	PW_KEY_NODE_NAME"=<string> "

struct pw_impl_node *
pw_adapter_new(struct pw_context *context,
		struct spa_node *follower,
		struct pw_properties *properties,
		size_t user_data_size);

void *pw_adapter_get_user_data(struct pw_impl_node *node);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_ADAPTER_H */
