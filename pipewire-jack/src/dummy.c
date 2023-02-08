/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <regex.h>
#include <math.h>

#include <pipewire/pipewire.h>

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	pw_init(NULL, NULL);
}
