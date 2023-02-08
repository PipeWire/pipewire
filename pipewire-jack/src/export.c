/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/defs.h>

#define JACK_METADATA_PREFIX "http://jackaudio.org/metadata/"
SPA_EXPORT const char *JACK_METADATA_CONNECTED   = JACK_METADATA_PREFIX "connected";
SPA_EXPORT const char *JACK_METADATA_EVENT_TYPES = JACK_METADATA_PREFIX "event-types";
SPA_EXPORT const char *JACK_METADATA_HARDWARE    = JACK_METADATA_PREFIX "hardware";
SPA_EXPORT const char *JACK_METADATA_ICON_LARGE  = JACK_METADATA_PREFIX "icon-large";
SPA_EXPORT const char *JACK_METADATA_ICON_NAME   = JACK_METADATA_PREFIX "icon-name";
SPA_EXPORT const char *JACK_METADATA_ICON_SMALL  = JACK_METADATA_PREFIX "icon-small";
SPA_EXPORT const char *JACK_METADATA_ORDER       = JACK_METADATA_PREFIX "order";
SPA_EXPORT const char *JACK_METADATA_PORT_GROUP  = JACK_METADATA_PREFIX "port-group";
SPA_EXPORT const char *JACK_METADATA_PRETTY_NAME = JACK_METADATA_PREFIX "pretty-name";
SPA_EXPORT const char *JACK_METADATA_SIGNAL_TYPE = JACK_METADATA_PREFIX "signal-type";
#undef JACK_METADATA_PREFIX
