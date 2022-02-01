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

#include "pwtest.h"

#include <pipewire/conf.h>

PWTEST(config_load_abspath)
{
	char path[PATH_MAX];
	int r;
	FILE *fp;
	struct pw_properties *props;
	char *basename;

	pwtest_mkstemp(path);
	fp = fopen(path, "w");
	fputs("data = x", fp);
	fclose(fp);

	/* Load with NULL prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf(NULL, path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

#if 0
	/* Load with non-NULL abs prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf("/dummy", path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

	/* Load with non-NULL relative prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf("dummy", path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);
#endif

	/* Load with non-NULL abs prefix and relative path */
	basename = rindex(path, '/'); /* basename(3) and dirname(3) are terrible */
	pwtest_ptr_notnull(basename);
	*basename = '\0';
	basename++;

	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf(path, basename, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST(config_load_nullname)
{
	struct pw_properties *props = pw_properties_new("ignore", "me", NULL);
	int r;

	r = pw_conf_load_conf(NULL, NULL, props);
	pwtest_neg_errno(r, -EINVAL);

	r = pw_conf_load_conf("/dummy", NULL, props);
	pwtest_neg_errno(r, -EINVAL);

	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST_SUITE(context)
{
	pwtest_add(config_load_abspath, PWTEST_NOARG);
	pwtest_add(config_load_nullname, PWTEST_NOARG);

	return PWTEST_PASS;
}
