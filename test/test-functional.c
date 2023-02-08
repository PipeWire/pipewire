/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include "pwtest.h"

PWTEST(openal_info_test)
{
#ifdef OPENAL_INFO_PATH
	int status = pwtest_spawn(OPENAL_INFO_PATH, (char *[]){ "openal-info", NULL });
	pwtest_int_eq(WEXITSTATUS(status), 0);
	return PWTEST_PASS;
#else
	return PWTEST_SKIP;
#endif
}

PWTEST(pactl_test)
{
#ifdef PACTL_PATH
	int status = pwtest_spawn(PACTL_PATH, (char *[]){ "pactl", "info", NULL });
	pwtest_int_eq(WEXITSTATUS(status), 0);
	return PWTEST_PASS;
#else
	return PWTEST_SKIP;
#endif
}

PWTEST_SUITE(pw_array)
{
	pwtest_add(pactl_test, PWTEST_ARG_DAEMON);
	pwtest_add(openal_info_test, PWTEST_ARG_DAEMON);

	return PWTEST_PASS;
}
