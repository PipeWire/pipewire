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

#ifndef PIPEWIRE_INTROSPECT_H
#define PIPEWIRE_INTROSPECT_H

#include <spa/utils/defs.h>
#include <spa/param/param.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/properties.h>

/** \enum pw_node_state The different node states \memberof pw_node */
enum pw_node_state {
	PW_NODE_STATE_ERROR = -1,	/**< error state */
	PW_NODE_STATE_CREATING = 0,	/**< the node is being created */
	PW_NODE_STATE_SUSPENDED = 1,	/**< the node is suspended, the device might
					 *   be closed */
	PW_NODE_STATE_IDLE = 2,		/**< the node is running but there is no active
					 *   port */
	PW_NODE_STATE_RUNNING = 3,	/**< the node is running */
};

/** Convert a \ref pw_node_state to a readable string \memberof pw_node */
const char * pw_node_state_as_string(enum pw_node_state state);

/** \enum pw_direction The direction of a port \memberof pw_introspect */
enum pw_direction {
	PW_DIRECTION_INPUT = SPA_DIRECTION_INPUT,	/**< an input port direction */
	PW_DIRECTION_OUTPUT = SPA_DIRECTION_OUTPUT	/**< an output port direction */
};

/** Convert a \ref pw_direction to a readable string \memberof pw_introspect */
const char * pw_direction_as_string(enum pw_direction direction);

/** \enum pw_link_state The different link states \memberof pw_link */
enum pw_link_state {
	PW_LINK_STATE_ERROR = -2,	/**< the link is in error */
	PW_LINK_STATE_UNLINKED = -1,	/**< the link is unlinked */
	PW_LINK_STATE_INIT = 0,		/**< the link is initialized */
	PW_LINK_STATE_NEGOTIATING = 1,	/**< the link is negotiating formats */
	PW_LINK_STATE_ALLOCATING = 2,	/**< the link is allocating buffers */
	PW_LINK_STATE_PAUSED = 3,	/**< the link is paused */
};

/** Convert a \ref pw_link_state to a readable string \memberof pw_link */
const char * pw_link_state_as_string(enum pw_link_state state);

/** \class pw_introspect
 *
 * The introspection methods and structures are used to get information
 * about the object in the PipeWire server
 */

/**  The core information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_core_info {
	uint32_t id;			/**< id of the global */
	uint32_t cookie;		/**< a random cookie for identifying this instance of PipeWire */
#define PW_CORE_CHANGE_MASK_USER_NAME  (1 << 0)
#define PW_CORE_CHANGE_MASK_HOST_NAME  (1 << 1)
#define PW_CORE_CHANGE_MASK_VERSION    (1 << 2)
#define PW_CORE_CHANGE_MASK_NAME       (1 << 3)
#define PW_CORE_CHANGE_MASK_PROPS      (1 << 4)
#define PW_CORE_CHANGE_MASK_ALL        (~0)
	uint64_t change_mask;		/**< bitfield of changed fields since last call */
	const char *user_name;		/**< name of the user that started the core */
	const char *host_name;		/**< name of the machine the core is running on */
	const char *version;		/**< version of the core */
	const char *name;		/**< name of the core */
	struct spa_dict *props;		/**< extra properties */
};

/** Update and existing \ref pw_core_info with \a update  \memberof pw_introspect */
struct pw_core_info *
pw_core_info_update(struct pw_core_info *info,
		    const struct pw_core_info *update);

/** Free a \ref pw_core_info  \memberof pw_introspect */
void pw_core_info_free(struct pw_core_info *info);

/** The module information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_module_info {
	uint32_t id;		/**< id of the global */
#define PW_MODULE_CHANGE_MASK_NAME	(1 << 0)
#define PW_MODULE_CHANGE_MASK_FILENAME	(1 << 1)
#define PW_MODULE_CHANGE_MASK_ARGS	(1 << 2)
#define PW_MODULE_CHANGE_MASK_PROPS	(1 << 3)
	uint64_t change_mask;	/**< bitfield of changed fields since last call */
	const char *name;	/**< name of the module */
	const char *filename;	/**< filename of the module */
	const char *args;	/**< arguments passed to the module */
	struct spa_dict *props;	/**< extra properties */
};

/** Update and existing \ref pw_module_info with \a update \memberof pw_introspect */
struct pw_module_info *
pw_module_info_update(struct pw_module_info *info,
		      const struct pw_module_info *update);

/** Free a \ref pw_module_info \memberof pw_introspect */
void pw_module_info_free(struct pw_module_info *info);


/** The device information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_device_info {
	uint32_t id;			/**< id of the global */
	const char *name;		/**< name the device */
#define PW_DEVICE_CHANGE_MASK_PROPS		(1 << 0)
#define PW_DEVICE_CHANGE_MASK_PARAMS		(1 << 1)
#define PW_DEVICE_CHANGE_MASK_ALL		((1 << 2)-1)
	uint64_t change_mask;		/**< bitfield of changed fields since last call */
	struct spa_dict *props;		/**< extra properties */
	struct spa_param_info *params;	/**< parameters */
	uint32_t n_params;		/**< number of items in \a params */
};

/** Update and existing \ref pw_device_info with \a update \memberof pw_introspect */
struct pw_device_info *
pw_device_info_update(struct pw_device_info *info,
		      const struct pw_device_info *update);

/** Free a \ref pw_device_info \memberof pw_introspect */
void pw_device_info_free(struct pw_device_info *info);

/** The client information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_client_info {
	uint32_t id;		/**< id of the global */
#define PW_CLIENT_CHANGE_MASK_PROPS		(1 << 0)
	uint64_t change_mask;	/**< bitfield of changed fields since last call */
	struct spa_dict *props;	/**< extra properties */
};

/** Update and existing \ref pw_client_info with \a update \memberof pw_introspect */
struct pw_client_info *
pw_client_info_update(struct pw_client_info *info,
		      const struct pw_client_info *update);

/** Free a \ref pw_client_info \memberof pw_introspect */
void pw_client_info_free(struct pw_client_info *info);


/** The node information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_node_info {
	uint32_t id;				/**< id of the global */
#define PW_NODE_CHANGE_MASK_NAME		(1 << 0)
#define PW_NODE_CHANGE_MASK_INPUT_PORTS		(1 << 1)
#define PW_NODE_CHANGE_MASK_OUTPUT_PORTS	(1 << 2)
#define PW_NODE_CHANGE_MASK_STATE		(1 << 3)
#define PW_NODE_CHANGE_MASK_PROPS		(1 << 4)
#define PW_NODE_CHANGE_MASK_PARAMS		(1 << 5)
#define PW_NODE_CHANGE_MASK_ALL			((1 << 6)-1)
	uint64_t change_mask;			/**< bitfield of changed fields since last call */
	const char *name;                       /**< name the node, suitable for display */
	uint32_t max_input_ports;		/**< maximum number of inputs */
	uint32_t n_input_ports;			/**< number of inputs */
	uint32_t max_output_ports;		/**< maximum number of outputs */
	uint32_t n_output_ports;		/**< number of outputs */
	enum pw_node_state state;		/**< the current state of the node */
	const char *error;			/**< an error reason if \a state is error */
	struct spa_dict *props;			/**< the properties of the node */
	struct spa_param_info *params;		/**< parameters */
	uint32_t n_params;			/**< number of items in \a params */
};

struct pw_node_info *
pw_node_info_update(struct pw_node_info *info,
		    const struct pw_node_info *update);

void
pw_node_info_free(struct pw_node_info *info);

struct pw_port_info {
	uint32_t id;				/**< id of the global */
	enum pw_direction direction;		/**< port direction */
#define PW_PORT_CHANGE_MASK_PROPS		(1 << 0)
#define PW_PORT_CHANGE_MASK_PARAMS		(1 << 1)
#define PW_PORT_CHANGE_MASK_ALL			((1 << 2)-1)
	uint64_t change_mask;			/**< bitfield of changed fields since last call */
	struct spa_dict *props;			/**< the properties of the port */
	struct spa_param_info *params;		/**< parameters */
	uint32_t n_params;			/**< number of items in \a params */
};

struct pw_port_info *
pw_port_info_update(struct pw_port_info *info,
		    const struct pw_port_info *update);

void
pw_port_info_free(struct pw_port_info *info);

/** The factory information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_factory_info {
	uint32_t id;			/**< id of the global */
	const char *name;		/**< name the factory */
	uint32_t type;			/**< type of the objects created by this factory */
	uint32_t version;		/**< version of the objects */
#define PW_FACTORY_CHANGE_MASK_PROPS	(1 << 0)
	uint64_t change_mask;		/**< bitfield of changed fields since last call */
	struct spa_dict *props;		/**< the properties of the factory */
};

struct pw_factory_info *
pw_factory_info_update(struct pw_factory_info *info,
		       const struct pw_factory_info *update);

void
pw_factory_info_free(struct pw_factory_info *info);

/** The link information. Extra information can be added in later versions \memberof pw_introspect */
struct pw_link_info {
	uint32_t id;			/**< id of the global */
#define PW_LINK_CHANGE_MASK_OUTPUT		(1 << 0)
#define PW_LINK_CHANGE_MASK_INPUT		(1 << 1)
#define PW_LINK_CHANGE_MASK_STATE		(1 << 2)
#define PW_LINK_CHANGE_MASK_FORMAT		(1 << 4)
#define PW_LINK_CHANGE_MASK_PROPS		(1 << 4)
	uint64_t change_mask;		/**< bitfield of changed fields since last call */
	uint32_t output_node_id;	/**< server side output node id */
	uint32_t output_port_id;	/**< output port id */
	uint32_t input_node_id;		/**< server side input node id */
	uint32_t input_port_id;		/**< input port id */
	enum pw_link_state state;	/**< the current state of the link */
	const char *error;		/**< an error reason if \a state is error */
	struct spa_pod *format;		/**< format over link */
	struct spa_dict *props;		/**< the properties of the link */
};

struct pw_link_info *
pw_link_info_update(struct pw_link_info *info,
		    const struct pw_link_info *update);

void
pw_link_info_free(struct pw_link_info *info);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_INTROSPECT_H */
