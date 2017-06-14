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

#include <pipewire/client/context.h>
#include <pipewire/client/introspect.h>
#include <pipewire/client/log.h>
#include <pipewire/client/loop.h>
#include <pipewire/client/mem.h>
#include <pipewire/client/thread-loop.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/stream.h>
#include <pipewire/client/subscribe.h>
#include <pipewire/client/utils.h>

#include <spa/type-map.h>

/** \mainpage
 *
 * \section sec_intro Introduction
 *
 * This document describes the API for the PipeWire multimedia server.
 * The API consists of two parts:
 *
 * \li The client side API (See \subpage page_client_api)
 * \li The server side API and tools to build new modules (See
 * \subpage page_server_api)
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
pw_fill_context_properties(struct pw_properties *properties);

void
pw_fill_stream_properties(struct pw_properties *properties);

enum pw_direction
pw_direction_reverse(enum pw_direction direction);

void *
pw_get_support(const char *type);

#ifdef __cplusplus
}
#endif
#endif /* __PIPEWIRE_H__ */
