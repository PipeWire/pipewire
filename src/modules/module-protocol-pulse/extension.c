/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <spa/utils/defs.h>
#include <spa/utils/string.h>

#include "defs.h"
#include "extension.h"
#include "extensions/registry.h"

static const struct extension extensions[] = {
	{ "module-stream-restore", 0 | MODULE_EXTENSION_FLAG, do_extension_stream_restore, },
	{ "module-device-restore", 1 | MODULE_EXTENSION_FLAG, do_extension_device_restore, },
	{ "module-device-manager", 2 | MODULE_EXTENSION_FLAG, do_extension_device_manager, },
};

const struct extension *extension_find(uint32_t index, const char *name)
{
	const struct extension *ext;

	SPA_FOR_EACH_ELEMENT(extensions, ext) {
		if (index == ext->index || spa_streq(name, ext->name))
			return ext;
	}

	return NULL;
}
