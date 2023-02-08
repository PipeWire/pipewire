/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_ENDPOINT_LINK_H
#define MODULE_SESSION_MANAGER_ENDPOINT_LINK_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct client_session;

struct endpoint_link {
	struct spa_list link;
	struct client_session *client_sess;
	struct pw_global *global;
	uint32_t id;			/* session-local link id */
	uint32_t n_params;
	struct spa_pod **params;
	struct pw_endpoint_link_info info;
	struct pw_properties *props;	/* wrapper of info.props */
};

int endpoint_link_init(struct endpoint_link *this,
		uint32_t id, uint32_t session_id,
		struct client_session *client_sess,
		struct pw_context *context,
		struct pw_properties *properties);

void endpoint_link_clear(struct endpoint_link *this);

int endpoint_link_update(struct endpoint_link *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_link_info *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_ENDPOINT_LINK_H */
