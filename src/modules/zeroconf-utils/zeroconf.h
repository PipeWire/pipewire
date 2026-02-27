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

#define PW_KEY_ZEROCONF_DISCOVER_LOCAL	"zeroconf.discover-local"	/* discover local services, true by default */

#define PW_KEY_ZEROCONF_IFINDEX		"zeroconf.ifindex"		/* interface index */
#define PW_KEY_ZEROCONF_PROTO		"zeroconf.proto"		/* protocol version, "4" ot "6" */
#define PW_KEY_ZEROCONF_NAME		"zeroconf.name"			/* session name */
#define PW_KEY_ZEROCONF_TYPE		"zeroconf.type"			/* service type, like "_http._tcp", not NULL */
#define PW_KEY_ZEROCONF_DOMAIN		"zeroconf.domain"		/* domain to register in, recommended NULL */
#define PW_KEY_ZEROCONF_HOST		"zeroconf.host"			/* host to register on, recommended NULL */
#define PW_KEY_ZEROCONF_SUBTYPES	"zeroconf.subtypes"		/* subtypes to register, array of strings */
#define PW_KEY_ZEROCONF_RESOLVE_PROTO	"zeroconf.resolve-proto"	/* protocol to resolve to, "4" or "6" */
#define PW_KEY_ZEROCONF_HOSTNAME	"zeroconf.hostname"		/* hostname of resolved service */
#define PW_KEY_ZEROCONF_PORT		"zeroconf.port"			/* port of resolved service */
#define PW_KEY_ZEROCONF_ADDRESS		"zeroconf.address"		/* address of resolved service */

struct pw_zeroconf;

struct pw_zeroconf_events {
#define PW_VERSION_ZEROCONF_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	void (*error) (void *data, int err, const char *message);

	void (*added) (void *data, const void *user, const struct spa_dict *info);
	void (*removed) (void *data, const void *user, const struct spa_dict *info);
};

struct pw_zeroconf * pw_zeroconf_new(struct pw_context *context,
				struct spa_dict *props);

void pw_zeroconf_destroy(struct pw_zeroconf *zc);

int pw_zeroconf_set_announce(struct pw_zeroconf *zc, const void *user, const struct spa_dict *info);
int pw_zeroconf_set_browse(struct pw_zeroconf *zc, const void *user, const struct spa_dict *info);

void pw_zeroconf_add_listener(struct pw_zeroconf *zc,
		struct spa_hook *listener,
		const struct pw_zeroconf_events *events, void *data);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_ZEROCONF_H */
