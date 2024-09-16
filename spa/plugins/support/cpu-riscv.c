/* Spa */
/* SPDX-FileCopyrightText: Copyright (c) 2023 Institue of Software Chinese Academy of Sciences (ISCAS). */
/* SPDX-License-Identifier: MIT */

#ifdef HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#define HWCAP_RV(letter) (1ul << ((letter) - 'A'))
#endif

static int
riscv_init(struct impl *impl)
{
	uint32_t flags = 0;

#ifdef HAVE_SYS_AUXV_H
	const unsigned long hwcap = getauxval(AT_HWCAP);
	if (hwcap & HWCAP_RV('V'))
		flags |= SPA_CPU_FLAG_RISCV_V;
#endif

	impl->flags = flags;

	return 0;
}

static int riscv_zero_denormals(void *object, bool enable)
{
	return 0;
}
