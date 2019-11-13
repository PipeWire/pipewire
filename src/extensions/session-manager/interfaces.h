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

#ifndef PIPEWIRE_EXT_SESSION_MANAGER_INTERFACES_H
#define PIPEWIRE_EXT_SESSION_MANAGER_INTERFACES_H

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

#include "introspect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PW_VERSION_SESSION_PROXY		0
struct pw_session_proxy { struct spa_interface iface; };
#define PW_VERSION_ENDPOINT_PROXY		0
struct pw_endpoint_proxy { struct spa_interface iface; };
#define PW_VERSION_ENDPOINT_STREAM_PROXY	0
struct pw_endpoint_stream_proxy { struct spa_interface iface; };
#define PW_VERSION_ENDPOINT_LINK_PROXY		0
struct pw_endpoint_link_proxy { struct spa_interface iface; };

/* Session */

#define PW_SESSION_PROXY_EVENT_INFO		0
#define PW_SESSION_PROXY_EVENT_PARAM		1
#define PW_SESSION_PROXY_EVENT_NUM		2

struct pw_session_proxy_events {
#define PW_VERSION_SESSION_PROXY_EVENTS		0
	uint32_t version;			/**< version of this structure */

	/**
	 * Notify session info
	 *
	 * \param info info about the session
	 */
	void (*info) (void *object, const struct pw_session_info *info);

	/**
	 * Notify a session param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

#define PW_SESSION_PROXY_METHOD_ADD_LISTENER		0
#define PW_SESSION_PROXY_METHOD_SUBSCRIBE_PARAMS	1
#define PW_SESSION_PROXY_METHOD_ENUM_PARAMS		2
#define PW_SESSION_PROXY_METHOD_SET_PARAM		3
#define PW_SESSION_PROXY_METHOD_CREATE_LINK		4
#define PW_SESSION_PROXY_METHOD_NUM			5

struct pw_session_proxy_methods {
#define PW_VERSION_SESSION_PROXY_METHODS	0
	uint32_t version;			/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_session_proxy_events *events,
			void *data);

	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate session parameters
	 *
	 * Start enumeration of session parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number returned in the reply
	 * \param id the parameter id to enumerate
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter);

	/**
	 * Set a parameter on the session
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

#define pw_session_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_session_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_session_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_session_proxy_add_listener(c,...)		pw_session_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_session_proxy_subscribe_params(c,...)	pw_session_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_session_proxy_enum_params(c,...)		pw_session_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_session_proxy_set_param(c,...)		pw_session_proxy_method(c,set_param,0,__VA_ARGS__)


/* Endpoint */

#define PW_ENDPOINT_PROXY_EVENT_INFO		0
#define PW_ENDPOINT_PROXY_EVENT_PARAM		1
#define PW_ENDPOINT_PROXY_EVENT_NUM		2

struct pw_endpoint_proxy_events {
#define PW_VERSION_ENDPOINT_PROXY_EVENTS	0
	uint32_t version;			/**< version of this structure */

	/**
	 * Notify endpoint info
	 *
	 * \param info info about the endpoint
	 */
	void (*info) (void *object, const struct pw_endpoint_info *info);

	/**
	 * Notify a endpoint param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

#define PW_ENDPOINT_PROXY_METHOD_ADD_LISTENER		0
#define PW_ENDPOINT_PROXY_METHOD_SUBSCRIBE_PARAMS	1
#define PW_ENDPOINT_PROXY_METHOD_ENUM_PARAMS		2
#define PW_ENDPOINT_PROXY_METHOD_SET_PARAM		3
#define PW_ENDPOINT_PROXY_METHOD_CREATE_LINK		4
#define PW_ENDPOINT_PROXY_METHOD_NUM			5

struct pw_endpoint_proxy_methods {
#define PW_VERSION_ENDPOINT_PROXY_METHODS	0
	uint32_t version;			/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_endpoint_proxy_events *events,
			void *data);

	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate endpoint parameters
	 *
	 * Start enumeration of endpoint parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number returned in the reply
	 * \param id the parameter id to enumerate
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter);

	/**
	 * Set a parameter on the endpoint
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	int (*create_link) (void *object, const struct spa_dict *props);
};

#define pw_endpoint_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_endpoint_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_endpoint_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_endpoint_proxy_add_listener(c,...)		pw_endpoint_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_endpoint_proxy_subscribe_params(c,...)	pw_endpoint_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_endpoint_proxy_enum_params(c,...)		pw_endpoint_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_endpoint_proxy_set_param(c,...)		pw_endpoint_proxy_method(c,set_param,0,__VA_ARGS__)
#define pw_endpoint_proxy_create_link(c,...)		pw_endpoint_proxy_method(c,create_link,0,__VA_ARGS__)

/* Endpoint Stream */

#define PW_ENDPOINT_STREAM_PROXY_EVENT_INFO		0
#define PW_ENDPOINT_STREAM_PROXY_EVENT_PARAM		1
#define PW_ENDPOINT_STREAM_PROXY_EVENT_NUM		2

struct pw_endpoint_stream_proxy_events {
#define PW_VERSION_ENDPOINT_STREAM_PROXY_EVENTS	0
	uint32_t version;			/**< version of this structure */

	/**
	 * Notify endpoint stream info
	 *
	 * \param info info about the endpoint stream
	 */
	void (*info) (void *object, const struct pw_endpoint_stream_info *info);

	/**
	 * Notify a endpoint stream param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

#define PW_ENDPOINT_STREAM_PROXY_METHOD_ADD_LISTENER		0
#define PW_ENDPOINT_STREAM_PROXY_METHOD_SUBSCRIBE_PARAMS	1
#define PW_ENDPOINT_STREAM_PROXY_METHOD_ENUM_PARAMS		2
#define PW_ENDPOINT_STREAM_PROXY_METHOD_SET_PARAM		3
#define PW_ENDPOINT_STREAM_PROXY_METHOD_NUM			4

struct pw_endpoint_stream_proxy_methods {
#define PW_VERSION_ENDPOINT_STREAM_PROXY_METHODS	0
	uint32_t version;			/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_endpoint_stream_proxy_events *events,
			void *data);

	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate stream parameters
	 *
	 * Start enumeration of stream parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number returned in the reply
	 * \param id the parameter id to enumerate
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter);

	/**
	 * Set a parameter on the stream
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

#define pw_endpoint_stream_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_endpoint_stream_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_endpoint_stream_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_endpoint_stream_proxy_add_listener(c,...)		pw_endpoint_stream_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_endpoint_stream_proxy_subscribe_params(c,...)	pw_endpoint_stream_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_endpoint_stream_proxy_enum_params(c,...)		pw_endpoint_stream_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_endpoint_stream_proxy_set_param(c,...)		pw_endpoint_stream_proxy_method(c,set_param,0,__VA_ARGS__)

/* Endpoint Link */

#define PW_ENDPOINT_LINK_PROXY_EVENT_INFO		0
#define PW_ENDPOINT_LINK_PROXY_EVENT_PARAM		1
#define PW_ENDPOINT_LINK_PROXY_EVENT_NUM		2

struct pw_endpoint_link_proxy_events {
#define PW_VERSION_ENDPOINT_LINK_PROXY_EVENTS	0
	uint32_t version;			/**< version of this structure */

	/**
	 * Notify endpoint link info
	 *
	 * \param info info about the endpoint link
	 */
	void (*info) (void *object, const struct pw_endpoint_link_info *info);

	/**
	 * Notify a endpoint link param
	 *
	 * Event emited as a result of the enum_params method.
	 *
	 * \param seq the sequence number of the request
	 * \param id the param id
	 * \param index the param index
	 * \param next the param index of the next param
	 * \param param the parameter
	 */
	void (*param) (void *object, int seq,
		       uint32_t id, uint32_t index, uint32_t next,
		       const struct spa_pod *param);
};

#define PW_ENDPOINT_LINK_PROXY_METHOD_ADD_LISTENER		0
#define PW_ENDPOINT_LINK_PROXY_METHOD_SUBSCRIBE_PARAMS		1
#define PW_ENDPOINT_LINK_PROXY_METHOD_ENUM_PARAMS		2
#define PW_ENDPOINT_LINK_PROXY_METHOD_SET_PARAM			3
#define PW_ENDPOINT_LINK_PROXY_METHOD_REQUEST_STATE		4
#define PW_ENDPOINT_LINK_PROXY_METHOD_DESTROY			5
#define PW_ENDPOINT_LINK_PROXY_METHOD_NUM			6

struct pw_endpoint_link_proxy_methods {
#define PW_VERSION_ENDPOINT_LINK_PROXY_METHODS	0
	uint32_t version;			/**< version of this structure */

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_endpoint_link_proxy_events *events,
			void *data);

	/**
	 * Subscribe to parameter changes
	 *
	 * Automatically emit param events for the given ids when
	 * they are changed.
	 *
	 * \param ids an array of param ids
	 * \param n_ids the number of ids in \a ids
	 */
	int (*subscribe_params) (void *object, uint32_t *ids, uint32_t n_ids);

	/**
	 * Enumerate link parameters
	 *
	 * Start enumeration of link parameters. For each param, a
	 * param event will be emited.
	 *
	 * \param seq a sequence number returned in the reply
	 * \param id the parameter id to enumerate
	 * \param start the start index or 0 for the first param
	 * \param num the maximum number of params to retrieve
	 * \param filter a param filter or NULL
	 */
	int (*enum_params) (void *object, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter);

	/**
	 * Set a parameter on the link
	 *
	 * \param id the parameter id to set
	 * \param flags extra parameter flags
	 * \param param the parameter to set
	 */
	int (*set_param) (void *object, uint32_t id, uint32_t flags,
			  const struct spa_pod *param);

	int (*request_state) (void *object, enum pw_endpoint_link_state state);
};

#define pw_endpoint_link_proxy_method(o,method,version,...)			\
({									\
	int _res = -ENOTSUP;						\
	struct pw_endpoint_link_proxy *_p = o;					\
	spa_interface_call_res(&_p->iface,				\
			struct pw_endpoint_link_proxy_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})

#define pw_endpoint_link_proxy_add_listener(c,...)	pw_endpoint_link_proxy_method(c,add_listener,0,__VA_ARGS__)
#define pw_endpoint_link_proxy_subscribe_params(c,...)	pw_endpoint_link_proxy_method(c,subscribe_params,0,__VA_ARGS__)
#define pw_endpoint_link_proxy_enum_params(c,...)	pw_endpoint_link_proxy_method(c,enum_params,0,__VA_ARGS__)
#define pw_endpoint_link_proxy_set_param(c,...)		pw_endpoint_link_proxy_method(c,set_param,0,__VA_ARGS__)
#define pw_endpoint_link_proxy_request_state(c,...)	pw_endpoint_link_proxy_method(c,request_state,0,__VA_ARGS__)


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_SESSION_MANAGER_INTERFACES_H */
