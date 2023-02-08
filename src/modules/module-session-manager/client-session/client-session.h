/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#ifndef MODULE_SESSION_MANAGER_CLIENT_SESSION_H
#define MODULE_SESSION_MANAGER_CLIENT_SESSION_H

#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

struct client_session {
	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
	struct session session;
	struct spa_list links;
};

#define pw_client_session_resource(r,m,v,...)	\
	pw_resource_call_res(r,struct pw_client_session_events,m,v,__VA_ARGS__)
#define pw_client_session_resource_set_id(r,...)	\
	pw_client_session_resource(r,set_id,0,__VA_ARGS__)
#define pw_client_session_resource_set_param(r,...)	\
	pw_client_session_resource(r,set_param,0,__VA_ARGS__)
#define pw_client_session_resource_link_set_param(r,...)	\
	pw_client_session_resource(r,link_set_param,0,__VA_ARGS__)
#define pw_client_session_resource_create_link(r,...)	\
	pw_client_session_resource(r,create_link,0,__VA_ARGS__)
#define pw_client_session_resource_destroy_link(r,...)	\
	pw_client_session_resource(r,destroy_link,0,__VA_ARGS__)
#define pw_client_session_resource_link_request_state(r,...)	\
	pw_client_session_resource(r,link_request_state,0,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MODULE_SESSION_MANAGER_CLIENT_SESSION_H */
