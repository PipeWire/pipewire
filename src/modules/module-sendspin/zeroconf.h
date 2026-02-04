/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_ZEROCONF_H
#define PIPEWIRE_ZEROCONF_H

#include <stdarg.h>

#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_zeroconf;

struct pw_zeroconf_events {
#define PW_VERSION_ZEROCONF_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	void (*error) (void *data, int err, const char *message);

	void (*added) (void *data, void *user, const struct spa_dict *info);
	void (*removed) (void *data, void *user, const struct spa_dict *info);
};

struct pw_zeroconf * pw_zeroconf_new(struct pw_context *context,
				struct spa_dict *props);

void pw_zeroconf_destroy(struct pw_zeroconf *zc);

int pw_zeroconf_set_announce(struct pw_zeroconf *zc, void *user, const struct spa_dict *info);
int pw_zeroconf_set_browse(struct pw_zeroconf *zc, void *user, const struct spa_dict *info);

void pw_zeroconf_add_listener(struct pw_zeroconf *zc,
		struct spa_hook *listener,
		const struct pw_zeroconf_events *events, void *data);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_ZEROCONF_H */
