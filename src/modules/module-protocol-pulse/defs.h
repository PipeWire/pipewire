/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_DEFS_H
#define PULSE_SERVER_DEFS_H

#include <pipewire/node.h>

#define FLAG_SHMDATA			0x80000000LU
#define FLAG_SHMDATA_MEMFD_BLOCK	0x20000000LU
#define FLAG_SHMRELEASE			0x40000000LU
#define FLAG_SHMREVOKE			0xC0000000LU
#define FLAG_SHMMASK			0xFF000000LU
#define FLAG_SEEKMASK			0x000000FFLU
#define FLAG_SHMWRITABLE		0x00800000LU

#define SEEK_RELATIVE		0
#define SEEK_ABSOLUTE		1
#define SEEK_RELATIVE_ON_READ	2
#define SEEK_RELATIVE_END	3

#define FRAME_SIZE_MAX_ALLOW (1024*1024*16)

#define PROTOCOL_FLAG_MASK	0xffff0000u
#define PROTOCOL_VERSION_MASK	0x0000ffffu
#define PROTOCOL_VERSION	35

#define NATIVE_COOKIE_LENGTH 256
#define MAX_TAG_SIZE (64*1024)

#define MIN_BUFFERS     1u
#define MAX_BUFFERS     4u

#define MAXLENGTH		(4u*1024*1024) /* 4MB */

#define SCACHE_ENTRY_SIZE_MAX	(1024*1024*16)

#define MODULE_INDEX_MASK	0xfffffffu
#define MODULE_FLAG		(1u << 29)

#define DEFAULT_SINK		"@DEFAULT_SINK@"
#define DEFAULT_SOURCE		"@DEFAULT_SOURCE@"
#define DEFAULT_MONITOR		"@DEFAULT_MONITOR@"

enum error_code {
	ERR_OK = 0,			/**< No error */
	ERR_ACCESS,			/**< Access failure */
	ERR_COMMAND,			/**< Unknown command */
	ERR_INVALID,			/**< Invalid argument */
	ERR_EXIST,			/**< Entity exists */
	ERR_NOENTITY,			/**< No such entity */
	ERR_CONNECTIONREFUSED,		/**< Connection refused */
	ERR_PROTOCOL,			/**< Protocol error */
	ERR_TIMEOUT,			/**< Timeout */
	ERR_AUTHKEY,			/**< No authentication key */
	ERR_INTERNAL,			/**< Internal error */
	ERR_CONNECTIONTERMINATED,	/**< Connection terminated */
	ERR_KILLED,			/**< Entity killed */
	ERR_INVALIDSERVER,		/**< Invalid server */
	ERR_MODINITFAILED,		/**< Module initialization failed */
	ERR_BADSTATE,			/**< Bad state */
	ERR_NODATA,			/**< No data */
	ERR_VERSION,			/**< Incompatible protocol version */
	ERR_TOOLARGE,			/**< Data too large */
	ERR_NOTSUPPORTED,		/**< Operation not supported \since 0.9.5 */
	ERR_UNKNOWN,			/**< The error code was unknown to the client */
	ERR_NOEXTENSION,		/**< Extension does not exist. \since 0.9.12 */
	ERR_OBSOLETE,			/**< Obsolete functionality. \since 0.9.15 */
	ERR_NOTIMPLEMENTED,		/**< Missing implementation. \since 0.9.15 */
	ERR_FORKED,			/**< The caller forked without calling execve() and tried to reuse the context. \since 0.9.15 */
	ERR_IO,				/**< An IO error happened. \since 0.9.16 */
	ERR_BUSY,			/**< Device or resource busy. \since 0.9.17 */
	ERR_MAX				/**< Not really an error but the first invalid error code */
};

static inline int res_to_err(int res)
{
	switch (res) {
	case 0: return ERR_OK;
	case -EACCES: case -EPERM: return ERR_ACCESS;
	case -ENOTTY: return ERR_COMMAND;
	case -EINVAL: return ERR_INVALID;
	case -EEXIST: return ERR_EXIST;
	case -ENOENT: case -ESRCH: case -ENXIO: case -ENODEV: return ERR_NOENTITY;
	case -ECONNREFUSED:
#ifdef ENONET
	case -ENONET:
#endif
	case -EHOSTDOWN: case -ENETDOWN: return ERR_CONNECTIONREFUSED;
	case -EPROTO: case -EBADMSG: return ERR_PROTOCOL;
	case -ETIMEDOUT:
#ifdef ETIME
	case -ETIME:
#endif
		return ERR_TIMEOUT;
#ifdef ENOKEY
	case -ENOKEY: return ERR_AUTHKEY;
#endif
	case -ECONNRESET: case -EPIPE: return ERR_CONNECTIONTERMINATED;
#ifdef EBADFD
	case -EBADFD: return ERR_BADSTATE;
#endif
#ifdef ENODATA
	case -ENODATA: return ERR_NODATA;
#endif
	case -EOVERFLOW: case -E2BIG: case -EFBIG:
	case -ERANGE: case -ENAMETOOLONG: return ERR_TOOLARGE;
	case -ENOTSUP: case -EPROTONOSUPPORT: case -ESOCKTNOSUPPORT: return ERR_NOTSUPPORTED;
	case -ENOSYS: return ERR_NOTIMPLEMENTED;
	case -EIO: return ERR_IO;
	case -EBUSY: case -EADDRINUSE: case -EAGAIN: return ERR_BUSY;
	case -ENFILE: case -EMFILE: case -ENOMEM: return ERR_INTERNAL;
	}
	return ERR_UNKNOWN;
}

static inline int err_to_res(int err)
{
	switch (err) {
	case ERR_OK: return 0;
	case ERR_ACCESS: return -EACCES;
	case ERR_COMMAND: return -ENOTTY;
	case ERR_INVALID: return -EINVAL;
	case ERR_EXIST: return -EEXIST;
	case ERR_NOENTITY: return -ENOENT;
	case ERR_CONNECTIONREFUSED: return -ECONNREFUSED;
	case ERR_PROTOCOL: return -EPROTO;
	case ERR_TIMEOUT: return -ETIMEDOUT;
#ifdef ENOKEY
	case ERR_AUTHKEY: return -ENOKEY;
#endif
	case ERR_INTERNAL: return -ENFILE;
	case ERR_CONNECTIONTERMINATED: return -ECONNRESET;
	case ERR_KILLED: return -EFAULT;
	case ERR_INVALIDSERVER: return -EINVAL;
	case ERR_MODINITFAILED: return -EIO;
#ifdef EBADFD
	case ERR_BADSTATE: return -EBADFD;
#endif
	case ERR_NODATA: return -ENODATA;
	case ERR_VERSION: return -EPROTO;
	case ERR_TOOLARGE: return -E2BIG;
	case ERR_NOTSUPPORTED: return -ENOTSUP;
	case ERR_UNKNOWN: return -EIO;
	case ERR_NOEXTENSION: return -ENOTTY;
	case ERR_OBSOLETE: return -ENOTSUP;
	case ERR_NOTIMPLEMENTED: return -ENOSYS;
	case ERR_FORKED: return -EIO;
	case ERR_IO: return -EIO;
	case ERR_BUSY: return -EBUSY;
	}
	return -EIO;
}

enum {
	SUBSCRIPTION_EVENT_SINK = 0x0000U,
	SUBSCRIPTION_EVENT_SOURCE = 0x0001U,
	SUBSCRIPTION_EVENT_SINK_INPUT = 0x0002U,
	SUBSCRIPTION_EVENT_SOURCE_OUTPUT = 0x0003U,
	SUBSCRIPTION_EVENT_MODULE = 0x0004U,
	SUBSCRIPTION_EVENT_CLIENT = 0x0005U,
	SUBSCRIPTION_EVENT_SAMPLE_CACHE = 0x0006U,
	SUBSCRIPTION_EVENT_SERVER = 0x0007U,
	// SUBSCRIPTION_EVENT_AUTOLOAD = 0x0008U,
	SUBSCRIPTION_EVENT_CARD = 0x0009U,
	SUBSCRIPTION_EVENT_FACILITY_MASK = 0x000FU,

	SUBSCRIPTION_EVENT_NEW = 0x0000U,
	SUBSCRIPTION_EVENT_CHANGE = 0x0010U,
	SUBSCRIPTION_EVENT_REMOVE = 0x0020U,
	SUBSCRIPTION_EVENT_TYPE_MASK = 0x0030U
};

enum {
	SUBSCRIPTION_MASK_NULL = 0,
	SUBSCRIPTION_MASK_SINK = 1u << SUBSCRIPTION_EVENT_SINK,
	SUBSCRIPTION_MASK_SOURCE = 1u << SUBSCRIPTION_EVENT_SOURCE,
	SUBSCRIPTION_MASK_SINK_INPUT = 1u << SUBSCRIPTION_EVENT_SINK_INPUT,
	SUBSCRIPTION_MASK_SOURCE_OUTPUT = 1u << SUBSCRIPTION_EVENT_SOURCE_OUTPUT,
	SUBSCRIPTION_MASK_MODULE = 1u << SUBSCRIPTION_EVENT_MODULE,
	SUBSCRIPTION_MASK_CLIENT = 1u << SUBSCRIPTION_EVENT_CLIENT,
	SUBSCRIPTION_MASK_SAMPLE_CACHE = 1u << SUBSCRIPTION_EVENT_SAMPLE_CACHE,
	SUBSCRIPTION_MASK_SERVER = 1u << SUBSCRIPTION_EVENT_SERVER,
	// SUBSCRIPTION_MASK_AUTOLOAD = 1u << SUBSCRIPTION_EVENT_AUTOLOAD,
	SUBSCRIPTION_MASK_CARD = 1u << SUBSCRIPTION_EVENT_CARD,
	SUBSCRIPTION_MASK_ALL =
		  SUBSCRIPTION_MASK_SINK
		| SUBSCRIPTION_MASK_SOURCE
		| SUBSCRIPTION_MASK_SINK_INPUT
		| SUBSCRIPTION_MASK_SOURCE_OUTPUT
		| SUBSCRIPTION_MASK_MODULE
		| SUBSCRIPTION_MASK_CLIENT
		| SUBSCRIPTION_MASK_SAMPLE_CACHE
		| SUBSCRIPTION_MASK_SERVER
		// | SUBSCRIPTION_MASK_AUTOLOAD
		| SUBSCRIPTION_MASK_CARD
};

enum {
	STATE_INVALID = -1,
	STATE_RUNNING = 0,
	STATE_IDLE = 1,
	STATE_SUSPENDED = 2,
	STATE_INIT = -2,
	STATE_UNLINKED = -3
};

static inline int node_state(enum pw_node_state state)
{
	switch (state) {
	case PW_NODE_STATE_ERROR:
		return STATE_UNLINKED;
	case PW_NODE_STATE_CREATING:
		return STATE_INIT;
	case PW_NODE_STATE_SUSPENDED:
		return STATE_SUSPENDED;
	case PW_NODE_STATE_IDLE:
		return STATE_IDLE;
	case PW_NODE_STATE_RUNNING:
		return STATE_RUNNING;
	}
	return STATE_INVALID;
}

enum {
	SINK_HW_VOLUME_CTRL = 0x0001U,
	SINK_LATENCY = 0x0002U,
	SINK_HARDWARE = 0x0004U,
	SINK_NETWORK = 0x0008U,
	SINK_HW_MUTE_CTRL = 0x0010U,
	SINK_DECIBEL_VOLUME = 0x0020U,
	SINK_FLAT_VOLUME = 0x0040U,
	SINK_DYNAMIC_LATENCY = 0x0080U,
	SINK_SET_FORMATS = 0x0100U,
};

enum {
	SOURCE_HW_VOLUME_CTRL = 0x0001U,
	SOURCE_LATENCY = 0x0002U,
	SOURCE_HARDWARE = 0x0004U,
	SOURCE_NETWORK = 0x0008U,
	SOURCE_HW_MUTE_CTRL = 0x0010U,
	SOURCE_DECIBEL_VOLUME = 0x0020U,
	SOURCE_DYNAMIC_LATENCY = 0x0040U,
	SOURCE_FLAT_VOLUME = 0x0080U,
};

static inline const char *subscription_event_type_to_string(uint32_t type)
{
	switch (type) {
	case SUBSCRIPTION_EVENT_NEW:
		return "new";
	case SUBSCRIPTION_EVENT_CHANGE:
		return "change";
	case SUBSCRIPTION_EVENT_REMOVE:
		return "remove";
	}

	return NULL;
}

static inline const char *subscription_event_facility_to_string(uint32_t facility)
{
	static const char * const strings[] = {
		[SUBSCRIPTION_EVENT_SINK] = "sink",
		[SUBSCRIPTION_EVENT_SOURCE] = "source",
		[SUBSCRIPTION_EVENT_SINK_INPUT] = "sink-input",
		[SUBSCRIPTION_EVENT_SOURCE_OUTPUT] = "source-output",
		[SUBSCRIPTION_EVENT_MODULE] = "module",
		[SUBSCRIPTION_EVENT_CLIENT] = "client",
		[SUBSCRIPTION_EVENT_SAMPLE_CACHE] = "sample-cache",
		[SUBSCRIPTION_EVENT_SERVER] = "server",
		// [SUBSCRIPTION_EVENT_AUTOLOAD] = "autoload",
		[SUBSCRIPTION_EVENT_CARD] = "card",
	};

	if (facility >= SPA_N_ELEMENTS(strings))
		return NULL;

	return strings[facility];
}

static const char * const port_types[] = {
	"unknown",
	"aux",
	"speaker",
	"headphones",
	"line",
	"mic",
	"headset",
	"handset",
	"earpiece",
	"spdif",
	"hdmi",
	"tv",
	"radio",
	"video",
	"usb",
	"bluetooth",
	"portable",
	"handsfree",
	"car",
	"hifi",
	"phone",
	"network",
	"analog",
};

static inline uint32_t port_type_value(const char *port_type)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(port_types); i++) {
		if (strcmp(port_types[i], port_type) == 0)
			return i;
	}
	return 0;
}

#define METADATA_DEFAULT_SINK           "default.audio.sink"
#define METADATA_DEFAULT_SOURCE         "default.audio.source"
#define METADATA_CONFIG_DEFAULT_SINK    "default.configured.audio.sink"
#define METADATA_CONFIG_DEFAULT_SOURCE  "default.configured.audio.source"
#define METADATA_TARGET_NODE            "target.node"
#define METADATA_TARGET_OBJECT          "target.object"

#endif /* PULSE_SERVER_DEFS_H */
