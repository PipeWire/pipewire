/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

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

PWTEST(init_deinit)
{
	pw_init(0, NULL);
	pw_deinit();
	pw_init(0, NULL);
	pw_init(0, NULL);
	pw_deinit();
	pw_deinit();
	return PWTEST_PASS;
}

PWTEST_SUITE(properties)
{
	pwtest_add(library_version, PWTEST_NOARG);
	pwtest_add(init_deinit, PWTEST_NOARG);

	return PWTEST_PASS;
}
