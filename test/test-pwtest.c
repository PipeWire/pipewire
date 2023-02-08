/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

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
