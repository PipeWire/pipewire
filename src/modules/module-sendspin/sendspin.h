/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_SENDSPIN_H
#define PIPEWIRE_SENDSPIN_H

#include <stdarg.h>

#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_SENDSPIN_SERVER_SERVICE	"_sendspin-server._tcp"
#define PW_SENDSPIN_CLIENT_SERVICE	"_sendspin._tcp"

#define PW_SENDSPIN_DEFAULT_SERVER_PORT	8927
#define PW_SENDSPIN_DEFAULT_CLIENT_PORT	8928
#define PW_SENDSPIN_DEFAULT_PATH	"/sendspin"

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_SENDSPIN_H */
