/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_ENDPOINT_H
#define MODULE_SESSION_MANAGER_ENDPOINT_H

#ifdef __cplusplus
extern "C" {
#endif

struct client_endpoint;

struct endpoint {
	struct client_endpoint *client_ep;
	struct pw_global *global;
	uint32_t n_params;
	struct spa_pod **params;
	struct pw_endpoint_info info;
	struct pw_properties *props;	/* wrapper of info.props */
};

int endpoint_init(struct endpoint *this,
		struct client_endpoint *client_ep,
		struct pw_context *context,
		struct pw_properties *properties);

void endpoint_clear(struct endpoint *this);

int endpoint_update(struct endpoint *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_info *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_ENDPOINT_H */
