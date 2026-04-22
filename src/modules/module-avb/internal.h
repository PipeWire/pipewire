/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_INTERNAL_H
#define AVB_INTERNAL_H

#include <sys/socket.h>

#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

struct server;
struct avb_gptp;
struct avb_mrp;

#define AVB_TSN_ETH 0x22f0
#define AVB_BROADCAST_MAC { 0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00 };

struct stream;

struct avb_transport_ops {
	int (*setup)(struct server *server);
	int (*send_packet)(struct server *server, const uint8_t dest[6],
			uint16_t type, void *data, size_t size);
	int (*make_socket)(struct server *server, uint16_t type,
			const uint8_t mac[6]);
	void (*destroy)(struct server *server);

	/* stream data plane ops */
	int (*stream_setup_socket)(struct server *server, struct stream *stream);
	ssize_t (*stream_send)(struct server *server, struct stream *stream,
			struct msghdr *msg, int flags);
};

struct impl {
	struct pw_loop *loop;
	struct pw_timer_queue *timer_queue;
	struct pw_context *context;
	struct spa_hook context_listener;
	struct pw_core *core;
	unsigned do_disconnect:1;

	struct pw_properties *props;

	struct spa_list servers;
};

struct server_events {
#define AVB_VERSION_SERVER_EVENTS	0
	uint32_t version;

	/** the server is destroyed */
	void (*destroy) (void *data);

	int (*message) (void *data, uint64_t now, const void *message, int len);

	void (*periodic) (void *data, uint64_t now);

	int (*command) (void *data, uint64_t now, const char *command, const char *args, FILE *out);

	void (*gm_changed) (void *data, uint64_t now, uint8_t gm_id[8]);
};

struct descriptor {
	struct spa_list link;
	uint16_t type;
	uint16_t index;
	uint32_t size;
	uint32_t state_size;
	void *ptr;
};

static inline void *descriptor_body(const struct descriptor *d)
{
	return SPA_PTROFF(d->ptr, d->state_size, void);
}


enum avb_mode {
	/** The legacy AVB Mode */
	AVB_MODE_LEGACY,
	/**
	 * \brief Milan version 1.2, which subset of the AVB,
	 * \see Milan Specifications https://avnu.org/resource/milan-specification/
	 */
	AVB_MODE_MILAN_V12,

	/** Future AVB mode will be added here if necessary */
	AVB_MODE_MAX
};

struct server {
	struct spa_list link;
	struct impl *impl;

	char *ifname;
	/** Parsed from the configuration pipewire-avb.conf */
	enum avb_mode avb_mode;
	uint8_t mac_addr[6];
	uint64_t entity_id;
	int ifindex;

	const struct avb_transport_ops *transport;
	void *transport_data;

	struct spa_source *source;
	struct pw_timer timer;

	struct spa_hook_list listener_list;

	struct spa_list descriptors;
	struct spa_list streams;

	unsigned debug_messages:1;

	struct avb_gptp *gptp;
	struct avb_mrp *mrp;
	struct avb_mmrp *mmrp;
	struct avb_mvrp *mvrp;
	struct avb_msrp *msrp;
	struct avb_maap *maap;
	struct avb_adp *adp;
	struct avb_acmp *acmp;

	struct avb_msrp_attribute *domain_attr;
};

#include "stream.h"

static inline void server_destroy_descriptors(struct server *server)
{
	struct descriptor *d, *t;

        spa_list_for_each_safe(d, t, &server->descriptors, link) {
		spa_list_remove(&d->link);
		free(d);
        }
}

static inline struct descriptor *server_find_descriptor(struct server *server,
		uint16_t type, uint16_t index)
{
	struct descriptor *d;
	spa_list_for_each(d, &server->descriptors, link) {
		if (d->type == type &&
		    d->index == index)
			return d;
	}
	return NULL;
}
static inline struct descriptor *server_add_descriptor(struct server *server,
		uint16_t type, uint16_t index,
		size_t state_size, size_t desc_size, const void *desc_ptr)
{
	struct descriptor *d;

	if ((d = calloc(1, sizeof(*d) + state_size + desc_size)) == NULL) {
		return NULL;
	}

	d->type = type;
	d->index = index;
	d->size = desc_size;
	d->state_size = state_size;
	d->ptr = SPA_PTROFF(d, sizeof(*d), void);
	if (desc_ptr) {
		memcpy(SPA_PTROFF(d->ptr, state_size, void), desc_ptr, desc_size);
	}
	spa_list_append(&server->descriptors, &d->link);
	return d;
}

const char *get_avb_mode_str(enum avb_mode mode);

struct server *avdecc_server_new(struct impl *impl, struct spa_dict *props);
void avdecc_server_free(struct server *server);

void avdecc_server_add_listener(struct server *server, struct spa_hook *listener,
		const struct server_events *events, void *data);

extern const struct avb_transport_ops avb_transport_raw;

int avb_server_make_socket(struct server *server, uint16_t type, const uint8_t mac[6]);

int avb_server_send_packet(struct server *server, const uint8_t dest[6],
		uint16_t type, void *data, size_t size);

int avb_server_stream_setup_socket(struct server *server, struct stream *stream);
ssize_t avb_server_stream_send(struct server *server, struct stream *stream,
		struct msghdr *msg, int flags);

void avb_log_state(struct server *server, const char *label);

struct aecp {
	struct server *server;
	struct spa_hook server_listener;
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AVB_INTERNAL_H */
