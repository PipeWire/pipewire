/* Spa */
/* SPDX-FileCopyrightText: Copyright (c) 2023 Institue of Software Chinese Academy of Sciences (ISCAS). */
/* SPDX-License-Identifier: MIT */

#if defined(HAVE_GETAUXVAL) || defined(HAVE_ELF_AUX_INFO)
#include <sys/auxv.h>
#define HWCAP_RV(letter) (1ul << ((letter) - 'A'))
#endif

static int
riscv_init(struct impl *impl)
{
	uint32_t flags = 0;

#if defined(HAVE_GETAUXVAL) || defined(HAVE_ELF_AUX_INFO)
#ifdef HAVE_GETAUXVAL
	const unsigned long hwcap = getauxval(AT_HWCAP);
#else
	unsigned long hwcap;

	if (elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap)) != 0)
		hwcap = 0;
#endif
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
