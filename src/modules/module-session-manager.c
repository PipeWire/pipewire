/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <pipewire/impl.h>

/** \page page_module_session_manager Session Manager
 *
 * This module implements some usefull objects for implementing a session
 * manager. It is not yet actively used.
 *
 * ## Module Name
 *
 * `libpipewire-module-session-manager`
 */

/* client-endpoint.c */
int client_endpoint_factory_init(struct pw_impl_module *module);
/* client-session.c */
int client_session_factory_init(struct pw_impl_module *module);

int session_factory_init(struct pw_impl_module *module);
int endpoint_factory_init(struct pw_impl_module *module);
int endpoint_stream_factory_init(struct pw_impl_module *module);
int endpoint_link_factory_init(struct pw_impl_module *module);

/* protocol-native.c */
int pw_protocol_native_ext_session_manager_init(struct pw_context *context);

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "George Kiagiadakis <george.kiagiadakis@collabora.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Implements objects for session management" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	int res;

	if ((res = pw_protocol_native_ext_session_manager_init(context)) < 0)
		return res;

	client_endpoint_factory_init(module);
	client_session_factory_init(module);
	session_factory_init(module);
	endpoint_factory_init(module);
	endpoint_stream_factory_init(module);
	endpoint_link_factory_init(module);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
