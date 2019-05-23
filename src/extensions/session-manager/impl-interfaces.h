/* PipeWire
 *
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_EXT_SESSION_MANAGER_IMPL_INTERFACES_H
#define PIPEWIRE_EXT_SESSION_MANAGER_IMPL_INTERFACES_H

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <errno.h>

#include "introspect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PW_VERSION_CLIENT_ENDPOINT_PROXY 0
struct pw_client_endpoint_proxy { struct spa_interface iface; };

#define PW_CLIENT_ENDPOINT_PROXY_EVENT_SET_ID			0
#define PW_CLIENT_ENDPOINT_PROXY_EVENT_SET_SESSION_ID		1
#define PW_CLIENT_ENDPOINT_PROXY_EVENT_SET_PARAM		2
#define PW_CLIENT_ENDPOINT_PROXY_EVENT_STREAM_SET_PARAM		3
#define PW_CLIENT_ENDPOINT_PROXY_EVENT_NUM			4

struct pw_client_endpoint_proxy_events {
#define PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS		0
	uint32_t version;		/**< version of this structure */

	/**
	 * Sets the id of the \a endpoint.
	 *
	 * On endpoint implementations, this is called by the server to notify
	 * the implementation of the assigned global id of the endpoint. The
	 * implementation is obliged to set this id in the
	 * #struct pw_endpoint_info \a id field. The implementation should also
	 * not emit the info() event before this method is called.
	 *
	 * \param endpoint a #pw_endpoint
	 * \param id the global id assigned to this endpoint
	 *
	 * \return 0 on success
	 *         -EINVAL when the id has already been set
	 *         -ENOTSUP on the server-side endpoint implementation
	 */
	int (*set_id) (void *endpoint, uint32_t id);

	/**
	 * Sets the session id of the \a endpoint.
	 *
	 * On endpoints that are not session masters, this method notifies
	 * the implementation that it has been associated with a session.
	 * The implementation is obliged to set this id in the
	 * #struct pw_endpoint_info \a session_id field.
	 *
	 * \param endpoint a #pw_endpoint
	 * \param id the session id associated with this endpoint
	 *
	 * \return 0 on success
	 *         -EINVAL when the session id has already been set
	 *         -ENOTSUP when the endpoint is a session master
	 */
	int (*set_session_id) (void *endpoint, uint32_t session_id);

	/**
	 * Set the configurable parameter in \a endpoint.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod
	 * as long as its keys and types match a supported object.
	 *
	 * Objects with property keys that are not known are ignored.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param endpoint a #struct pw_endpoint
	 * \param id the parameter id to configure
	 * \param flags additional flags
	 * \param param the parameter to configure
	 *
	 * \return 0 on success
	 *         -EINVAL when \a endpoint is NULL
	 *         -ENOTSUP when there are no parameters implemented on \a endpoint
	 *         -ENOENT the parameter is unknown
	 */
	int (*set_param) (void *endpoint,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	/**
	 * Set a parameter on \a stream_id of \a endpoint.
	 *
	 * When \a param is NULL, the parameter will be unset.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param endpoint a #struct pw_endpoint
	 * \param stream_id the stream to configure
	 * \param id the parameter id to set
	 * \param flags optional flags
	 * \param param a #struct spa_pod with the parameter to set
	 * \return 0 on success
	 *         1 on success, the value of \a param might have been
	 *                changed depending on \a flags and the final value can
	 *                be found by doing stream_enum_params.
	 *         -EINVAL when \a endpoint is NULL or invalid arguments are given
	 *         -ESRCH when the type or size of a property is not correct.
	 *         -ENOENT when the param id is not found
	 */
	int (*stream_set_param) (void *endpoint, uint32_t stream_id,
			         uint32_t id, uint32_t flags,
			         const struct spa_pod *param);
};

#define PW_CLIENT_ENDPOINT_PROXY_METHOD_ADD_LISTENER	0
#define PW_CLIENT_ENDPOINT_PROXY_METHOD_UPDATE		1
#define PW_CLIENT_ENDPOINT_PROXY_METHOD_STREAM_UPDATE	2
#define PW_CLIENT_ENDPOINT_PROXY_METHOD_NUM		3

struct pw_client_endpoint_proxy_methods {
#define PW_VERSION_CLIENT_ENDPOINT_PROXY_METHODS 	0
	uint32_t version;		/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_client_endpoint_proxy_events *events,
			void *data);

	/** Update endpoint information */
	int (*update) (void *object,
#define PW_CLIENT_ENDPOINT_UPDATE_PARAMS	(1 << 0)
#define PW_CLIENT_ENDPOINT_UPDATE_INFO		(1 << 1)
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_info *info);

	/** Update stream information */
	int (*stream_update) (void *object,
				uint32_t stream_id,
#define PW_CLIENT_ENDPOINT_STREAM_UPDATE_PARAMS		(1 << 0)
#define PW_CLIENT_ENDPOINT_STREAM_UPDATE_INFO		(1 << 1)
#define PW_CLIENT_ENDPOINT_STREAM_UPDATE_DESTROYED	(1 << 2)
				uint32_t change_mask,
				uint32_t n_params,
				const struct spa_pod **params,
				const struct pw_endpoint_stream_info *info);
};

#define pw_client_endpoint_proxy_method(o,method,version,...)		\
({									\
	int _res = -ENOTSUP;						\
	struct pw_client_endpoint_proxy *_p = o;			\
	spa_interface_call_res(&_p->iface,				\
			struct pw_client_endpoint_proxy_methods, _res,	\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_client_endpoint_proxy_add_listener(o,...)	pw_client_endpoint_proxy_method(o,add_listener,0,__VA_ARGS__)
#define pw_client_endpoint_proxy_update(o,...)		pw_client_endpoint_proxy_method(o,update,0,__VA_ARGS__)
#define pw_client_endpoint_proxy_stream_update(o,...)	pw_client_endpoint_proxy_method(o,stream_update,0,__VA_ARGS__)


#define PW_VERSION_CLIENT_SESSION_PROXY 0
struct pw_client_session_proxy { struct spa_interface iface; };

#define PW_CLIENT_SESSION_PROXY_EVENT_SET_ID			0
#define PW_CLIENT_SESSION_PROXY_EVENT_SET_PARAM			1
#define PW_CLIENT_SESSION_PROXY_EVENT_LINK_SET_PARAM		2
#define PW_CLIENT_SESSION_PROXY_EVENT_CREATE_LINK		3
#define PW_CLIENT_SESSION_PROXY_EVENT_DESTROY_LINK		4
#define PW_CLIENT_SESSION_PROXY_EVENT_LINK_REQUEST_STATE	5
#define PW_CLIENT_SESSION_PROXY_EVENT_NUM			6

struct pw_client_session_proxy_events {
#define PW_VERSION_CLIENT_SESSION_PROXY_EVENTS		0
	uint32_t version;		/**< version of this structure */

	/**
	 * Sets the id of the \a session.
	 *
	 * On session implementations, this is called by the server to notify
	 * the implementation of the assigned global id of the session. The
	 * implementation is obliged to set this id in the
	 * #struct pw_session_info \a id field. The implementation should also
	 * not emit the info() event before this method is called.
	 *
	 * \param session a #pw_session
	 * \param id the global id assigned to this session
	 *
	 * \return 0 on success
	 *         -EINVAL when the id has already been set
	 *         -ENOTSUP on the server-side session implementation
	 */
	int (*set_id) (void *session, uint32_t id);

	/**
	 * Set the configurable parameter in \a session.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod
	 * as long as its keys and types match a supported object.
	 *
	 * Objects with property keys that are not known are ignored.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param session a #struct pw_session
	 * \param id the parameter id to configure
	 * \param flags additional flags
	 * \param param the parameter to configure
	 *
	 * \return 0 on success
	 *         -EINVAL when \a session is NULL
	 *         -ENOTSUP when there are no parameters implemented on \a session
	 *         -ENOENT the parameter is unknown
	 */
	int (*set_param) (void *session,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	/**
	 * Set a parameter on \a link_id of \a session.
	 *
	 * When \a param is NULL, the parameter will be unset.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param session a #struct pw_session
	 * \param link_id the link to configure
	 * \param id the parameter id to set
	 * \param flags optional flags
	 * \param param a #struct spa_pod with the parameter to set
	 * \return 0 on success
	 *         1 on success, the value of \a param might have been
	 *                changed depending on \a flags and the final value can
	 *                be found by doing link_enum_params.
	 *         -EINVAL when \a session is NULL or invalid arguments are given
	 *         -ESRCH when the type or size of a property is not correct.
	 *         -ENOENT when the param id is not found
	 */
	int (*link_set_param) (void *session, uint32_t link_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param);

	int (*create_link) (void *session, const struct spa_dict *props);

	int (*destroy_link) (void *session, uint32_t link_id);

	int (*link_request_state) (void *session, uint32_t link_id, uint32_t state);
};

#define PW_CLIENT_SESSION_PROXY_METHOD_ADD_LISTENER	0
#define PW_CLIENT_SESSION_PROXY_METHOD_UPDATE		1
#define PW_CLIENT_SESSION_PROXY_METHOD_LINK_UPDATE	2
#define PW_CLIENT_SESSION_PROXY_METHOD_NUM		3

struct pw_client_session_proxy_methods {
#define PW_VERSION_CLIENT_SESSION_PROXY_METHODS 	0
	uint32_t version;		/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_client_session_proxy_events *events,
			void *data);

	/** Update session information */
	int (*update) (void *object,
#define PW_CLIENT_SESSION_UPDATE_PARAMS		(1 << 0)
#define PW_CLIENT_SESSION_UPDATE_INFO		(1 << 1)
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_session_info *info);

	/** Update link information */
	int (*link_update) (void *object,
				uint32_t link_id,
#define PW_CLIENT_SESSION_LINK_UPDATE_PARAMS		(1 << 0)
#define PW_CLIENT_SESSION_LINK_UPDATE_INFO		(1 << 1)
#define PW_CLIENT_SESSION_LINK_UPDATE_DESTROYED		(1 << 2)
				uint32_t change_mask,
				uint32_t n_params,
				const struct spa_pod **params,
				const struct pw_endpoint_link_info *info);
};

#define pw_client_session_proxy_method(o,method,version,...)		\
({									\
	int _res = -ENOTSUP;						\
	struct pw_client_session_proxy *_p = o;				\
	spa_interface_call_res(&_p->iface,				\
			struct pw_client_session_proxy_methods, _res,	\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_client_session_proxy_add_listener(o,...)	pw_client_session_proxy_method(o,add_listener,0,__VA_ARGS__)
#define pw_client_session_proxy_update(o,...)		pw_client_session_proxy_method(o,update,0,__VA_ARGS__)
#define pw_client_session_proxy_link_update(o,...)	pw_client_session_proxy_method(o,link_update,0,__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_SESSION_MANAGER_IMPL_INTERFACES_H */
