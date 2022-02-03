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

#include <signal.h>

#include "pwtest-compat.c"

PWTEST(compat_sigabbrev_np)
{
#ifndef HAVE_SIGABBREV_NP
	pwtest_str_eq(sigabbrev_np(SIGABRT), "ABRT");
	pwtest_str_eq(sigabbrev_np(SIGSEGV), "SEGV");
	pwtest_str_eq(sigabbrev_np(SIGSTOP), "STOP");
	pwtest_str_eq(sigabbrev_np(SIGCHLD), "CHLD");
	pwtest_str_eq(sigabbrev_np(SIGTERM), "TERM");
	pwtest_str_eq(sigabbrev_np(SIGKILL), "KILL");
	pwtest_str_eq(sigabbrev_np(12345), NULL);

	return PWTEST_PASS;
#else
	return PWTEST_SKIP;
#endif
}

PWTEST_SUITE(pwtest)
{
	pwtest_add(compat_sigabbrev_np, PWTEST_NOARG);

	return PWTEST_PASS;
}
