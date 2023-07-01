/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_AVB_H
#define PIPEWIRE_AVB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_context;
struct pw_properties;
struct pw_avb;

struct pw_avb *pw_avb_new(struct pw_context *context,
		struct pw_properties *props, size_t user_data_size);
void pw_avb_destroy(struct pw_avb *avb);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_AVB_H */
