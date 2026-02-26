/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <avahi-client/client.h>

#include <pipewire/context.h>

AvahiPoll* pw_avahi_poll_new(struct pw_context *context);

void pw_avahi_poll_free(AvahiPoll *p);
