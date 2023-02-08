/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <avahi-client/client.h>

#include <pipewire/loop.h>

AvahiPoll* pw_avahi_poll_new(struct pw_loop *loop);

void pw_avahi_poll_free(AvahiPoll *p);
