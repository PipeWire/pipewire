/* PipeWire
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#ifndef __PIPEWIRE_EXTENSION_H__
#define __PIPEWIRE_EXTENSION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/client/context.h>

#define PIPEWIRE_SYMBOL_EXTENSION_INIT "pipewire__extension_init"

/** \class pw_extension
 *
 * A dynamically loadable extension
 */
struct pw_extension {
	struct pw_context *context;	/**< the client context */
	struct spa_list link;		/**< link in the context extension_list */

	const char *filename;		/**< filename of extension */
	const char *args;		/**< argument for the extension */
	struct pw_properties *props;	/**< extra properties */

	void *user_data;		/**< extension user_data */

	/** Emited when the extension is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_extension *extension));
};

/** Module init function signature \memberof pw_extension
 *
 * \param extension A \ref pw_extension
 * \param args Arguments to the extension
 * \return true on success, false otherwise
 *
 * A extension should provide an init function with this signature. This function
 * will be called when a extension is loaded.
 */
typedef bool (*pw_extension_init_func_t) (struct pw_extension *extension, char *args);

struct pw_extension *
pw_extension_load(struct pw_context *context,
		  const char *name, const char *args);

void
pw_extension_destroy(struct pw_extension *extension);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_EXTENSION_H__ */
