/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#ifndef HAVE_SIGABBREV_NP
#include <stddef.h>
#include <signal.h>

/* glibc >= 2.32 */
static inline const char *sigabbrev_np(int sig)
{
#define SIGABBREV(a_) case SIG##a_: return #a_
	switch(sig) {
	SIGABBREV(INT);
	SIGABBREV(ABRT);
	SIGABBREV(BUS);
	SIGABBREV(SEGV);
	SIGABBREV(ALRM);
	SIGABBREV(CHLD);
	SIGABBREV(HUP);
	SIGABBREV(PIPE);
	SIGABBREV(CONT);
	SIGABBREV(STOP);
	SIGABBREV(ILL);
	SIGABBREV(KILL);
	SIGABBREV(TERM);
	}
#undef SIGABBREV

	return NULL;
}

#endif /* HAVE_SIGABBREV_NP */
