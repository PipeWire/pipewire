/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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


#ifndef SM_MEDIA_SESSION_H
#define SM_MEDIA_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

struct sm_media_session;

struct sm_object_events {
#define SM_VERSION_OBJECT_EVENTS	0
	uint32_t version;

	void (*update) (void *data);
	void (*destroy) (void *data);
};

struct sm_object {
	uint32_t id;
	uint32_t type;

	struct spa_list link;
	struct sm_media_session *session;

#define SM_OBJECT_CHANGE_MASK_LISTENER		(1<<1)
#define SM_OBJECT_CHANGE_MASK_PROPERTIES	(1<<2)
#define SM_OBJECT_CHANGE_MASK_BIND		(1<<3)
#define SM_OBJECT_CHANGE_MASK_LAST		(1<<8)
	uint32_t mask;			/**< monitored info */
	uint32_t avail;			/**< available info */
	uint32_t changed;		/**< changed since last update */
	struct pw_properties *props;	/**< global properties */

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
	pw_destroy_t destroy;

	struct spa_hook_list hooks;

	struct spa_list data;
};

int sm_object_add_listener(struct sm_object *obj, struct spa_hook *listener,
		const struct sm_object_events *events, void *data);

struct sm_param {
	uint32_t id;
	struct spa_list link;		/**< link in param_list */
	struct spa_pod *param;
};

/** get user data with \a id and \a size to an object */
void *sm_object_add_data(struct sm_object *obj, const char *id, size_t size);
void *sm_object_get_data(struct sm_object *obj, const char *id);
int sm_object_remove_data(struct sm_object *obj, const char *id);

int sm_object_destroy(struct sm_object *obj);

struct sm_client {
	struct sm_object obj;

#define SM_CLIENT_CHANGE_MASK_INFO		(SM_OBJECT_CHANGE_MASK_LAST<<0)
#define SM_CLIENT_CHANGE_MASK_PERMISSIONS	(SM_OBJECT_CHANGE_MASK_LAST<<1)
	struct pw_client_info *info;
};

struct sm_device {
	struct sm_object obj;

	unsigned int subscribe:1;	/**< if we subscribed to param changes */

#define SM_DEVICE_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
#define SM_DEVICE_CHANGE_MASK_PARAMS	(SM_OBJECT_CHANGE_MASK_LAST<<1)
#define SM_DEVICE_CHANGE_MASK_NODES	(SM_OBJECT_CHANGE_MASK_LAST<<2)
	uint32_t n_params;
	struct spa_list param_list;	/**< list of sm_param */
	struct pw_device_info *info;
	struct spa_list node_list;
};


struct sm_node {
	struct sm_object obj;

	struct sm_device *device;	/**< optional device */
	struct spa_list link;		/**< link in device node_list */
	unsigned int subscribe:1;	/**< if we subscribed to param changes */
	uint32_t last_id;

#define SM_NODE_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
#define SM_NODE_CHANGE_MASK_PARAMS	(SM_OBJECT_CHANGE_MASK_LAST<<1)
#define SM_NODE_CHANGE_MASK_PORTS	(SM_OBJECT_CHANGE_MASK_LAST<<2)
	uint32_t n_params;
	struct spa_list param_list;	/**< list of sm_param */
	struct pw_node_info *info;
	struct spa_list port_list;
};

struct sm_port {
	struct sm_object obj;

	enum pw_direction direction;
	struct sm_node *node;
	struct spa_list link;		/**< link in node port_list */

#define SM_PORT_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
	struct pw_port_info *info;
};

struct sm_session {
	struct sm_object obj;

#define SM_SESSION_CHANGE_MASK_INFO		(SM_OBJECT_CHANGE_MASK_LAST<<0)
#define SM_SESSION_CHANGE_MASK_ENDPOINTS	(SM_OBJECT_CHANGE_MASK_LAST<<1)
	struct pw_session_info *info;
	struct spa_list endpoint_list;
};

struct sm_endpoint {
	struct sm_object obj;

	int32_t priority;

	struct sm_session *session;
	struct spa_list link;		/**< link in session endpoint_list */

#define SM_ENDPOINT_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
#define SM_ENDPOINT_CHANGE_MASK_STREAMS	(SM_OBJECT_CHANGE_MASK_LAST<<1)
	struct pw_endpoint_info *info;
	struct spa_list stream_list;
};

struct sm_endpoint_stream {
	struct sm_object obj;

	int32_t priority;

	struct sm_endpoint *endpoint;
	struct spa_list link;		/**< link in endpoint stream_list */

	struct spa_list link_list;	/**< list of links */

#define SM_ENDPOINT_STREAM_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
	struct pw_endpoint_stream_info *info;
};

struct sm_endpoint_link {
	struct sm_object obj;

	struct spa_list link;		/**< link in session link_list */

	struct spa_list output_link;
	struct sm_endpoint_stream *output;
	struct spa_list input_link;
	struct sm_endpoint_stream *input;

#define SM_ENDPOINT_LINK_CHANGE_MASK_INFO	(SM_OBJECT_CHANGE_MASK_LAST<<0)
	struct pw_endpoint_link_info *info;
};

struct sm_media_session_events {
#define SM_VERSION_MEDIA_SESSION_EVENTS	0
	uint32_t version;

	void (*create) (void *data, struct sm_object *object);
	void (*remove) (void *data, struct sm_object *object);

	void (*rescan) (void *data, int seq);
};

struct sm_media_session {
	struct sm_session *session;	/** session object managed by this session */

	struct pw_loop *loop;		/** the main loop */
	struct pw_context *context;

	struct spa_dbus_connection *dbus_connection;
};

int sm_media_session_add_listener(struct sm_media_session *sess, struct spa_hook *listener,
		const struct sm_media_session_events *events, void *data);

int sm_media_session_roundtrip(struct sm_media_session *sess);

int sm_media_session_sync(struct sm_media_session *sess,
		void (*callback) (void *data), void *data);

struct sm_object *sm_media_session_find_object(struct sm_media_session *sess, uint32_t id);

int sm_media_session_schedule_rescan(struct sm_media_session *sess);

struct pw_proxy *sm_media_session_export(struct sm_media_session *sess,
		uint32_t type, struct pw_properties *properties,
		void *object, size_t user_data_size);

struct sm_device *sm_media_session_export_device(struct sm_media_session *sess,
		struct pw_properties *properties, struct spa_device *device);

struct pw_proxy *sm_media_session_create_object(struct sm_media_session *sess,
		const char *factory_name, uint32_t type, uint32_t version,
		const struct spa_dict *props, size_t user_data_size);

struct sm_node *sm_media_session_create_node(struct sm_media_session *sess,
		const char *factory_name, const struct spa_dict *props);

int sm_media_session_create_links(struct sm_media_session *sess,
		const struct spa_dict *dict);

#ifdef __cplusplus
}
#endif

#endif
