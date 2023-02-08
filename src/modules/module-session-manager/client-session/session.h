/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_SESSION_H
#define MODULE_SESSION_MANAGER_SESSION_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct client_session;

struct session {
	struct client_session *client_sess;
	struct pw_global *global;
	uint32_t n_params;
	struct spa_pod **params;
	struct pw_session_info info;
	struct pw_properties *props;	/* wrapper of info.props */
};

int session_init(struct session *this,
		struct client_session *client_sess,
		struct pw_context *context,
		struct pw_properties *properties);

void session_clear(struct session *this);

int session_update(struct session *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_session_info *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_SESSION_H */
