/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_CLIENT_ENDPOINT_H
#define MODULE_SESSION_MANAGER_CLIENT_ENDPOINT_H

#include "endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

struct client_endpoint {
	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
	struct endpoint endpoint;
	struct spa_list streams;
};

#define pw_client_endpoint_resource(r,m,v,...)	\
	pw_resource_call_res(r,struct pw_client_endpoint_events,m,v,__VA_ARGS__)
#define pw_client_endpoint_resource_set_id(r,...)	\
	pw_client_endpoint_resource(r,set_id,0,__VA_ARGS__)
#define pw_client_endpoint_resource_set_session_id(r,...)	\
	pw_client_endpoint_resource(r,set_session_id,0,__VA_ARGS__)
#define pw_client_endpoint_resource_set_param(r,...)	\
	pw_client_endpoint_resource(r,set_param,0,__VA_ARGS__)
#define pw_client_endpoint_resource_stream_set_param(r,...)	\
	pw_client_endpoint_resource(r,stream_set_param,0,__VA_ARGS__)
#define pw_client_endpoint_resource_create_link(r,...)	\
	pw_client_endpoint_resource(r,create_link,0,__VA_ARGS__)

int client_endpoint_factory_init(struct pw_impl_module *module);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_CLIENT_ENDPOINT_H */
