/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_PLUGIN_H__
#define __SPA_PLUGIN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>

#define SPA_TYPE__Handle		SPA_TYPE_INTERFACE_BASE "Handle"
#define SPA_TYPE__HandleFactory		SPA_TYPE_INTERFACE_BASE "HandleFactory"

struct spa_handle {
	/** Version of this struct */
#define SPA_VERSION_HANDLE	0
	uint32_t version;

	/* user_data that can be set by the application */
	void *user_data;
	/**
	 * Get the interface provided by \a handle with \a interface_id.
	 *
	 * \param handle a spa_handle
	 * \param interface_id the interface id
	 * \param interface result to hold the interface.
	 * \return 0 on success
	 *         -ENOTSUP when there are no interfaces
	 *         -EINVAL when handle or info is NULL
	 */
	int (*get_interface) (struct spa_handle *handle, uint32_t interface_id, void **interface);
	/**
	 * Clean up the memory of \a handle. After this, \a handle should not be used
	 * anymore.
	 *
	 * \param handle a pointer to memory
	 * \return 0 on success
	 */
	int (*clear) (struct spa_handle *handle);
};

#define spa_handle_get_interface(h,...)	(h)->get_interface((h),__VA_ARGS__)
#define spa_handle_clear(h)		(h)->clear((h))

/**
 * This structure lists the information about available interfaces on
 * handles.
 */
struct spa_interface_info {
	const char *type;	/*< the type of the interface, can be
				 *  used to get the interface */
};

/**
 * Extra supporting infrastructure passed to the init() function of
 * a factory. It can be extra information or interfaces such as logging.
 */
struct spa_support {
	const char *type;	/*< the type of the support item */
	void *data;		/*< specific data for the item */
};

/** Find a support item of the given type */
static inline void *spa_support_find(const struct spa_support *support,
				     uint32_t n_support,
				     const char *type)
{
	uint32_t i;
	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, type) == 0)
			return support[i].data;
	}
	return NULL;
}

#define SPA_SUPPORT_INIT(type,data) (struct spa_support) { (type), (data) }

struct spa_handle_factory {
	/** The version of this structure */
#define SPA_VERSION_HANDLE_FACTORY	0
	uint32_t version;
	/**
	 * The name
	 */
	const char *name;
	/**
	 * Extra information about the handles of this factory.
	 */
	const struct spa_dict *info;
	/**
	 * The size of handles from this factory
	 */
	const size_t size;

	/**
	 * Initialize an instance of this factory. The caller should allocate
	 * memory at least size bytes and pass this as \a handle.
	 *
	 * \a support can optionally contain extra interfaces or data items that the
	 * plugin can use such as a logger.
	 *
	 * \param factory a spa_handle_factory
	 * \param handle a pointer to memory
	 * \param info extra handle specific information, usually obtained
	 *        from a spa_monitor. This can be used to configure the handle.
	 * \param support support items
	 * \param n_support number of elements in \a support
	 * \return 0 on success
	 *	   < 0 errno type error
	 */
	int (*init) (const struct spa_handle_factory *factory,
		     struct spa_handle *handle,
		     const struct spa_dict *info,
		     const struct spa_support *support,
		     uint32_t n_support);

	/**
	 * spa_handle_factory::enum_interface_info:
	 * \param factory: a #spa_handle_factory
	 * \param info: result to hold spa_interface_info.
	 * \param index: index to keep track of the enumeration, 0 for first item
	 *
	 * Enumerate the interface information for \a factory.
	 *
	 * \return 1 when an item is available
	 *	   0 when no more items are available
	 *	   < 0 errno type error
	 */
	int (*enum_interface_info) (const struct spa_handle_factory *factory,
				    const struct spa_interface_info **info,
				    uint32_t *index);
};

#define spa_handle_factory_init(h,...)			(h)->init((h),__VA_ARGS__)
#define spa_handle_factory_enum_interface_info(h,...)	(h)->enum_interface_info((h),__VA_ARGS__)

/**
 * The function signature of the entry point in a plugin.
 *
 * \param factory a location to hold the factory result
 * \param index index to keep track of the enumeration
 * \return 1 on success
 *         0 when there are no more factories
 *         -EINVAL when factory is NULL
 */
typedef int (*spa_handle_factory_enum_func_t) (const struct spa_handle_factory **factory,
					       uint32_t *index);

#define SPA_HANDLE_FACTORY_ENUM_FUNC_NAME "spa_handle_factory_enum"

/**
 * The entry point in a plugin.
 *
 * \param factory a location to hold the factory result
 * \param index index to keep track of the enumeration
 * \return 1 on success
 *	   0 when no more items are available
 *	   < 0 errno type error
 */
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PLUGIN_H__ */
