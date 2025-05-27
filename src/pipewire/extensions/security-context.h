/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_EXT_SECURITY_CONTEXT_H
#define PIPEWIRE_EXT_SECURITY_CONTEXT_H

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup pw_security_context Security Context
 * Security Context interface
 */

/**
 * \addtogroup pw_security_context
 * \{
 */
#define PW_TYPE_INTERFACE_SecurityContext		PW_TYPE_INFO_INTERFACE_BASE "SecurityContext"

#define PW_SECURITY_CONTEXT_PERM_MASK			PW_PERM_RWX

#define PW_VERSION_SECURITY_CONTEXT			3
struct pw_security_context;

#ifndef PW_API_SECURITY_CONTEXT
#define PW_API_SECURITY_CONTEXT static inline
#endif

#define PW_EXTENSION_MODULE_SECURITY_CONTEXT		PIPEWIRE_MODULE_PREFIX "module-security-context"

#define PW_SECURITY_CONTEXT_EVENT_NUM			0


/** \ref pw_security_context events */
struct pw_security_context_events {
#define PW_VERSION_SECURITY_CONTEXT_EVENTS		0
	uint32_t version;
};

#define PW_SECURITY_CONTEXT_METHOD_ADD_LISTENER		0
#define PW_SECURITY_CONTEXT_METHOD_CREATE		1
#define PW_SECURITY_CONTEXT_METHOD_NUM			2

/** \ref pw_security_context methods */
struct pw_security_context_methods {
#define PW_VERSION_SECURITY_CONTEXT_METHODS		0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_security_context_events *events,
			void *data);

	/**
	 * Create a new security context
	 *
	 * Creates a new security context with a socket listening FD.
	 * PipeWire will accept new client connections on listen_fd.
	 *
	 * listen_fd must be ready to accept new connections when this request is
	 * sent by the client. In other words, the client must call bind(2) and
	 * listen(2) before sending the FD.
	 *
	 * close_fd is a FD closed by the client when PipeWire should stop
	 * accepting new connections on listen_fd.
	 *
	 * PipeWire must continue to accept connections on listen_fd when
	 * the client which created the security context disconnects.
	 *
	 * After sending this request, closing listen_fd and close_fd remains the
	 * only valid operation on them.
	 *
	 * \param listen_fd the fd to listen on for new connections
	 * \param close_fd the fd used to stop listening
	 * \param props extra properties. These will be copied on the client
	 *     that connects through this context.
	 *
	 * Some properties to set:
	 *
	 *  - pipewire.sec.engine with the engine name.
	 *  - pipewire.sec.app-id with the application id, this is an opaque,
	 *      engine specific id for an application
	 *  - pipewire.sec.instance-id with the instance id, this is an opaque,
	 *      engine specific id for a running instance of an application.
	 *
	 * See https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/staging/security-context/engines.md
	 * For a list of engine names and the properties to set.
	 *
	 * This requires X and W permissions on the security_context.
	 */
	int (*create) (void *object,
			int listen_fd,
			int close_fd,
			const struct spa_dict *props);
};

/** \copydoc pw_security_context_methods.add_listener
 * \sa pw_security_context_methods.add_listener */
PW_API_SECURITY_CONTEXT int pw_security_context_add_listener(struct pw_security_context *object,
			struct spa_hook *listener,
			const struct pw_security_context_events *events,
			void *data)
{
	return spa_api_method_r(int, -ENOTSUP,
			pw_security_context, (struct spa_interface*)object, add_listener, 0,
			listener, events, data);
}

/** \copydoc pw_security_context_methods.create
 * \sa pw_security_context_methods.create */
PW_API_SECURITY_CONTEXT int pw_security_context_create(struct pw_security_context *object,
			int listen_fd, int close_fd, const struct spa_dict *props)
{
	return spa_api_method_r(int, -ENOTSUP,
			pw_security_context, (struct spa_interface*)object, create, 0,
			listen_fd, close_fd, props);
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_SECURITY_CONTEXT_H */
