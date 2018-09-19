/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PIPEWIRE_H__
#define __PIPEWIRE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/plugin.h>

#include <pipewire/client.h>
#include <pipewire/core.h>
#include <pipewire/interfaces.h>
#include <pipewire/introspect.h>
#include <pipewire/link.h>
#include <pipewire/log.h>
#include <pipewire/loop.h>
#include <pipewire/main-loop.h>
#include <pipewire/module.h>
#include <pipewire/factory.h>
#include <pipewire/node.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/remote.h>
#include <pipewire/resource.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <pipewire/type.h>
#include <pipewire/utils.h>
#include <pipewire/version.h>

/** \mainpage
 *
 * \section sec_intro Introduction
 *
 * This document describes the API for the PipeWire multimedia framework.
 * The API consists of two parts:
 *
 * \li The core API and tools to build new modules (See
 * \subpage page_core_api)
 * \li The remote API (See \subpage page_remote_api)
 *
 * \section sec_errors Error reporting
 *
 * Functions return either NULL or a negative int error code when an
 * error occurs. Error codes are used from the SPA plugin library on
 * which PipeWire is built.
 *
 * Some functions might return asynchronously. The error code for such
 * functions is positive and SPA_RESULT_IS_ASYNC() will return true.
 * SPA_RESULT_ASYNC_SEQ() can be used to get the unique sequence number
 * associated with the async operation.
 *
 * The object returning the async result code will have some way to
 * signal the completion of the async operation (with, for example, a
 * callback). The sequence number can be used to see which operation
 * completed.
 *
 * \section sec_logging Logging
 *
 * The 'PIPEWIRE_DEBUG' environment variable can be used to enable
 * more debugging. The format is:
 *
 *    &lt;level&gt;[:&lt;category&gt;,...]
 *
 * - &lt;level&gt;: specifies the log level:
 *   + `0`: no logging is enabled
 *   + `1`: Error logging is enabled
 *   + `2`: Warnings are enabled
 *   + `3`: Informational messages are enabled
 *   + `4`: Debug messages are enabled
 *   + `5`: Trace messages are enabled. These messages can be logged
 *		from the realtime threads.
 *
 * - &lt;category&gt;:  Specifies a string category to enable. Many categories
 *		  can be separated by commas. Current categories are:
 *   + `connection`: to log connection messages
 */

/** \class pw_pipewire
 *
 * \brief PipeWire initalization and infrasctructure functions
 */
void
pw_init(int *argc, char **argv[]);

bool
pw_debug_is_category_enabled(const char *name);

const char *
pw_get_application_name(void);

const char *
pw_get_prgname(void);

const char *
pw_get_user_name(void);

const char *
pw_get_host_name(void);

char *
pw_get_client_name(void);

void
pw_fill_remote_properties(struct pw_core *core, struct pw_properties *properties);

void
pw_fill_stream_properties(struct pw_core *core, struct pw_properties *properties);

enum pw_direction
pw_direction_reverse(enum pw_direction direction);

void *
pw_get_support_interface(const char *type);

void *pw_get_spa_dbus(struct pw_loop *loop);
int pw_release_spa_dbus(void *dbus);

const struct spa_handle_factory *
pw_get_support_factory(const char *factory_name);

const struct spa_support *
pw_get_support(uint32_t *n_support);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_H__ */
