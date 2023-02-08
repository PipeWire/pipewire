/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_PROTOCOL_PULSE_H
#define PIPEWIRE_PROTOCOL_PULSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

#define PW_PROTOCOL_PULSE_DEFAULT_PORT 4713
#define PW_PROTOCOL_PULSE_DEFAULT_SOCKET "native"

#define PW_PROTOCOL_PULSE_DEFAULT_SERVER "unix:"PW_PROTOCOL_PULSE_DEFAULT_SOCKET

#define PW_PROTOCOL_PULSE_USAGE	"[ server.address=(tcp:[<ip>:]<port>|unix:<path>)[,...] ] "		\

struct pw_context;
struct pw_properties;
struct pw_protocol_pulse;
struct pw_protocol_pulse_server;

struct pw_protocol_pulse *pw_protocol_pulse_new(struct pw_context *context,
		struct pw_properties *props, size_t user_data_size);
void *pw_protocol_pulse_get_user_data(struct pw_protocol_pulse *pulse);
void pw_protocol_pulse_destroy(struct pw_protocol_pulse *pulse);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_PROTOCOL_PULSE_H */
