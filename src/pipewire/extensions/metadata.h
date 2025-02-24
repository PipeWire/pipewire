/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_EXT_METADATA_H
#define PIPEWIRE_EXT_METADATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

#include <errno.h>

/** \defgroup pw_metadata Metadata
 * Metadata interface
 */

/**
 * \addtogroup pw_metadata
 * \{
 */
#define PW_TYPE_INTERFACE_Metadata		PW_TYPE_INFO_INTERFACE_BASE "Metadata"

#define PW_METADATA_PERM_MASK			PW_PERM_RWX

#define PW_VERSION_METADATA			3
struct pw_metadata;

#ifndef PW_API_METADATA_IMPL
#define PW_API_METADATA_IMPL static inline
#endif

#define PW_EXTENSION_MODULE_METADATA		PIPEWIRE_MODULE_PREFIX "module-metadata"

#define PW_METADATA_EVENT_PROPERTY		0
#define PW_METADATA_EVENT_NUM			1


/** \ref pw_metadata events */
struct pw_metadata_events {
#define PW_VERSION_METADATA_EVENTS		0
	uint32_t version;

	int (*property) (void *data,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value);
};

#define PW_METADATA_METHOD_ADD_LISTENER		0
#define PW_METADATA_METHOD_SET_PROPERTY		1
#define PW_METADATA_METHOD_CLEAR		2
#define PW_METADATA_METHOD_NUM			3

/** \ref pw_metadata methods */
struct pw_metadata_methods {
#define PW_VERSION_METADATA_METHODS		0
	uint32_t version;

	int (*add_listener) (void *object,
			struct spa_hook *listener,
			const struct pw_metadata_events *events,
			void *data);

	/**
	 * Set a metadata property
	 *
	 * Automatically emit property events for the subject and key
	 * when they are changed.
	 *
	 * \param subject the id of the global to associate the metadata
	 *                with.
	 * \param key the key of the metadata, NULL clears all metadata for
	 *                the subject.
	 * \param type the type of the metadata, this can be blank
	 * \param value the metadata value. NULL clears the metadata.
	 *
	 * This requires X and W permissions on the metadata. It also
	 * requires M permissions on the subject global.
	 */
	int (*set_property) (void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value);

	/**
	 * Clear all metadata
	 *
	 * This requires X and W permissions on the metadata.
	 */
	int (*clear) (void *object);
};

/** \copydoc pw_metadata_methods.add_listener
 * \sa pw_metadata_methods.add_listener */
PW_API_METADATA_IMPL int pw_metadata_add_listener(struct pw_metadata *object,
			struct spa_hook *listener,
			const struct pw_metadata_events *events,
			void *data)
{
	return spa_api_method_r(int, -ENOTSUP,
			pw_metadata, (struct spa_interface*)object, add_listener, 0,
			listener, events, data);
}
/** \copydoc pw_metadata_methods.set_property
 * \sa pw_metadata_methods.set_property */
PW_API_METADATA_IMPL int pw_metadata_set_property(struct pw_metadata *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	return spa_api_method_r(int, -ENOTSUP,
			pw_metadata, (struct spa_interface*)object, set_property, 0,
			subject, key, type, value);
}
/** \copydoc pw_metadata_methods.clear
 * \sa pw_metadata_methods.clear */
PW_API_METADATA_IMPL int pw_metadata_clear(struct pw_metadata *object)
{
	return spa_api_method_r(int, -ENOTSUP,
			pw_metadata, (struct spa_interface*)object, clear, 0);
}

#define PW_KEY_METADATA_NAME		"metadata.name"
#define PW_KEY_METADATA_VALUES		"metadata.values"

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_EXT_METADATA_H */
