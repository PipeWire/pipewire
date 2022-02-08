/* PipeWire
 *
 * Copyright © 2021 Red Hat, Inc.
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


#include "config.h"

#include "pwtest.h"

#include "pipewire/pipewire.h"

PWTEST(library_version)
{
	const char *libversion, *headerversion;
	char version_expected[64];

	pw_init(0, NULL);
	libversion = pw_get_library_version();
	headerversion = pw_get_headers_version();

	spa_scnprintf(version_expected, sizeof(version_expected),
		"%d.%d.%d", PW_MAJOR, PW_MINOR, PW_MICRO);

	pwtest_str_eq(headerversion, version_expected);
	pwtest_str_eq(libversion, version_expected);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(properties)
{
	pwtest_add(library_version, PWTEST_NOARG);

	return PWTEST_PASS;
}
