/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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
