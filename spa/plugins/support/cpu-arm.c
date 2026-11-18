/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(HAVE_GETAUXVAL) || defined(HAVE_ELF_AUX_INFO)
#include <sys/auxv.h>
#endif

#include <spa/utils/string.h>

#if defined(HAVE_GETAUXVAL) || defined(HAVE_ELF_AUX_INFO)
#if !defined(__aarch64__)
/* glibc uses HWCAP_ARM_* names on 32-bit ARM */
#if !defined(HWCAP_VFP) && defined(HWCAP_ARM_VFP)
#define HWCAP_VFP	HWCAP_ARM_VFP
#endif
#if !defined(HWCAP_NEON) && defined(HWCAP_ARM_NEON)
#define HWCAP_NEON	HWCAP_ARM_NEON
#endif
#if !defined(HWCAP_VFPv3) && defined(HWCAP_ARM_VFPv3)
#define HWCAP_VFPv3	HWCAP_ARM_VFPv3
#endif
#endif
#else
#define MAX_BUFFER 4096

static char *get_cpuinfo_line(char *cpuinfo, const char *tag)
{
	char *line, *end, *colon;

	if (!(line = strstr(cpuinfo, tag)))
		return NULL;

	if (!(end = strchr(line, '\n')))
		return NULL;

	if (!(colon = strchr(line, ':')))
		return NULL;

	if (++colon >= end)
		return NULL;

	return strndup(colon, end - colon);
}
#endif

static int
arm_init(struct impl *impl)
{
	uint32_t flags = 0;
#if defined(HAVE_GETAUXVAL) || defined(HAVE_ELF_AUX_INFO)
	unsigned long hwcap;

#ifdef HAVE_GETAUXVAL
	hwcap = getauxval(AT_HWCAP);
#else
	if (elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap)) != 0)
		hwcap = 0;
#endif

#if defined(__aarch64__)
	flags |= SPA_CPU_FLAG_ARMV6 | SPA_CPU_FLAG_ARMV8;
	if (hwcap & HWCAP_ASIMD)
		flags |= SPA_CPU_FLAG_NEON;
	if (hwcap & HWCAP_FP)
		flags |= SPA_CPU_FLAG_VFPV3 | SPA_CPU_FLAG_VFP;
#else
#if __ARM_ARCH >= 6
	flags |= SPA_CPU_FLAG_ARMV6;
#endif
#if __ARM_ARCH >= 8
	flags |= SPA_CPU_FLAG_ARMV8;
#endif
#ifdef HWCAP_VFP
	if (hwcap & HWCAP_VFP)
		flags |= SPA_CPU_FLAG_VFP;
#endif
#ifdef HWCAP_NEON
	if (hwcap & HWCAP_NEON)
		flags |= SPA_CPU_FLAG_NEON;
#endif
#ifdef HWCAP_VFPv3
	if (hwcap & HWCAP_VFPv3)
		flags |= SPA_CPU_FLAG_VFPV3;
#endif
#endif
#else
	char *cpuinfo, *line, buffer[MAX_BUFFER];
	int arch;

	if (!(cpuinfo = spa_cpu_read_file("/proc/cpuinfo", buffer, sizeof(buffer)))) {
		spa_log_warn(impl->log, "%p: Can't read cpuinfo", impl);
		return 1;
	}

	if ((line = get_cpuinfo_line(cpuinfo, "CPU architecture"))) {
		arch = strtoul(line, NULL, 0);
		if (arch >= 6)
			flags |= SPA_CPU_FLAG_ARMV6;
		if (arch >= 8)
			flags |= SPA_CPU_FLAG_ARMV8;

		free(line);
	}

	if ((line = get_cpuinfo_line(cpuinfo, "Features"))) {
		char *state = NULL;
		char *current = strtok_r(line, " ", &state);

		do {
#if defined (__aarch64__)
			if (spa_streq(current, "asimd"))
				flags |= SPA_CPU_FLAG_NEON;
			else if (spa_streq(current, "fp"))
				flags |= SPA_CPU_FLAG_VFPV3 | SPA_CPU_FLAG_VFP;
#else
			if (spa_streq(current, "vfp"))
				flags |= SPA_CPU_FLAG_VFP;
			else if (spa_streq(current, "neon"))
				flags |= SPA_CPU_FLAG_NEON;
			else if (spa_streq(current, "vfpv3"))
				flags |= SPA_CPU_FLAG_VFPV3;
#endif
		} while ((current = strtok_r(NULL, " ", &state)));

		free(line);
	}
#endif

	impl->flags = flags;

	return 0;
}


static int arm_zero_denormals(void *object, bool enable)
{
#if defined(__aarch64__)
	uint64_t cw;
	if (enable)
		__asm__ __volatile__(
			"mrs	%0, fpcr		\n"
			"orr	%0, %0, #0x1000000	\n"
			"msr	fpcr, %0		\n"
			"isb				\n"
			: "=r"(cw)::"memory");
	else
		__asm__ __volatile__(
			"mrs	%0, fpcr		\n"
			"and	%0, %0, #~0x1000000	\n"
			"msr	fpcr, %0		\n"
			"isb				\n"
			: "=r"(cw)::"memory");
#elif (defined(__VFP_FP__) && !defined(__SOFTFP__))
	uint32_t cw;
	if (enable)
		__asm__ __volatile__(
			"vmrs	%0, fpscr		\n"
			"orr	%0, %0, #0x1000000	\n"
			"vmsr	fpscr, %0		\n"
			: "=r"(cw)::"memory");
	else
		__asm__ __volatile__(
			"vmrs	%0, fpscr		\n"
			"and	%0, %0, #~0x1000000	\n"
			"vmsr	fpscr, %0		\n"
			: "=r"(cw)::"memory");
#endif
	return 0;
}
