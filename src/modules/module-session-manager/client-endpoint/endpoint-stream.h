/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_ENDPOINT_STREAM_H
#define MODULE_SESSION_MANAGER_ENDPOINT_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

struct client_endpoint;

struct endpoint_stream {
	struct spa_list link;
	struct client_endpoint *client_ep;
	struct pw_global *global;
	uint32_t id;			/* endpoint-local stream id */
	uint32_t n_params;
	struct spa_pod **params;
	struct pw_endpoint_stream_info info;
	struct pw_properties *props;	/* wrapper of info.props */
};

int endpoint_stream_init(struct endpoint_stream *this,
		uint32_t id, uint32_t endpoint_id,
		struct client_endpoint *client_ep,
		struct pw_context *context,
		struct pw_properties *properties);

void endpoint_stream_clear(struct endpoint_stream *this);

int endpoint_stream_update(struct endpoint_stream *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_stream_info *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_ENDPOINT_STREAM_H */
