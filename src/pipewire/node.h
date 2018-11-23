/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#ifndef __PIPEWIRE_NODE_H__
#define __PIPEWIRE_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \page page_node Node
 *
 * \section page_node_overview Overview
 *
 * The node object processes data. The node has a list of
 * input and output ports (\ref page_port) on which it
 * will receive and send out buffers respectively.
 */
/** \class pw_node
 *
 * PipeWire node class.
 */
struct pw_node;

#include <pipewire/core.h>
#include <pipewire/global.h>
#include <pipewire/introspect.h>
#include <pipewire/port.h>
#include <pipewire/resource.h>

/** Node events, listen to them with \ref pw_node_add_listener */
struct pw_node_events {
#define PW_VERSION_NODE_EVENTS	0
	uint32_t version;

	/** the node is destroyed */
        void (*destroy) (void *data);
	/** the node is about to be freed */
        void (*free) (void *data);
	/** the node is initialized */
        void (*initialized) (void *data);

	/** a port is being initialized on the node */
        void (*port_init) (void *data, struct pw_port *port);
	/** a port was added */
        void (*port_added) (void *data, struct pw_port *port);
	/** a port was removed */
        void (*port_removed) (void *data, struct pw_port *port);

	/** the node info changed */
	void (*info_changed) (void *data, struct pw_node_info *info);
	/** the node active state changed */
	void (*active_changed) (void *data, bool active);
	/** the node enabled state changed */
	void (*enabled_changed) (void *data, bool enabled);

	/** a new state is requested on the node */
	void (*state_request) (void *data, enum pw_node_state state);
	/** the state of the node changed */
	void (*state_changed) (void *data, enum pw_node_state old,
			       enum pw_node_state state, const char *error);

	/** an async operation completed on the node */
	void (*async_complete) (void *data, uint32_t seq, int res);

        /** an event is emited */
	void (*event) (void *data, const struct spa_event *event);

	/** the driver of the node changed */
	void (*driver_changed) (void *data, struct pw_node *driver);
};

/** Media type of the node, Audio, Video, Midi */
#define PW_NODE_PROP_MEDIA		"pipewire.media"
/** Category: Playback, Capture, Duplex */
#define PW_NODE_PROP_CATEGORY		"pipewire.category"
/** Role: Movie,Music, Camera, Screen, Communication, Game, Notification, DSP,
 *        Production, Accessibility, Test */
#define PW_NODE_PROP_ROLE		"pipewire.role"
/** exclusive access to device */
#define PW_NODE_PROP_EXCLUSIVE		"pipewire.exclusive"
/** Automatically connect this node to a compatible node */
#define PW_NODE_PROP_AUTOCONNECT	"pipewire.autoconnect"
/** Try to connect the node to this node id */
#define PW_NODE_PROP_TARGET_NODE	"pipewire.target.node"

/** Create a new node \memberof pw_node */
struct pw_node *
pw_node_new(struct pw_core *core,		/**< the core */
	    const char *name,			/**< node name */
	    struct pw_properties *properties,	/**< extra properties */
	    size_t user_data_size		/**< user data size */);

/** Complete initialization of the node and register */
int pw_node_register(struct pw_node *node,		/**< node to register */
		     struct pw_client *owner,		/**< optional owner */
		     struct pw_global *parent,		/**< optional parent */
		     struct pw_properties *properties	/**< extra properties */);

/** Destroy a node */
void pw_node_destroy(struct pw_node *node);

/** Get the node info */
const struct pw_node_info *pw_node_get_info(struct pw_node *node);

/** Get node user_data. The size of the memory was given in \ref pw_node_new */
void * pw_node_get_user_data(struct pw_node *node);

/** Get the core of this node */
struct pw_core *pw_node_get_core(struct pw_node *node);

/** Get the global of this node */
struct pw_global *pw_node_get_global(struct pw_node *node);

/** Get the node properties */
const struct pw_properties *pw_node_get_properties(struct pw_node *node);

/** Update the node properties */
int pw_node_update_properties(struct pw_node *node, const struct spa_dict *dict);

/** Set the node implementation */
void pw_node_set_implementation(struct pw_node *node, struct spa_node *spa_node);
/** Get the node implementation */
struct spa_node *pw_node_get_implementation(struct pw_node *node);

/** Add an event listener */
void pw_node_add_listener(struct pw_node *node,
			  struct spa_hook *listener,
			  const struct pw_node_events *events,
			  void *data);

/** Iterate the ports in the given direction. The callback should return
 * 0 to fetch the next item, any other value stops the iteration and returns
 * the value. When all callbacks return 0, this function returns 0 when all
 * items are iterated. */
int pw_node_for_each_port(struct pw_node *node,
			  enum pw_direction direction,
			  int (*callback) (void *data, struct pw_port *port),
			  void *data);

int pw_node_for_each_param(struct pw_node *node,
			   uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data);

/** Find the port with direction and port_id or NULL when not found. Passing
 * SPA_ID_INVALID for port_id will return any port, preferably an unlinked one. */
struct pw_port *
pw_node_find_port(struct pw_node *node, enum pw_direction direction, uint32_t port_id);

/** Get a free unused port_id from the node */
uint32_t pw_node_get_free_port_id(struct pw_node *node, enum pw_direction direction);

/** Set a node active. This will start negotiation with all linked active
  * nodes and start data transport */
int pw_node_set_active(struct pw_node *node, bool active);

/** Check if a node is active */
bool pw_node_is_active(struct pw_node *node);

/** Set a node enabled. The node will be able to be activated */
int pw_node_set_enabled(struct pw_node *node, bool enabled);

/** Check if a node is enabled */
bool pw_node_is_enabled(struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_NODE_H__ */
