/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <spa/utils/string.h>

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

static int
arm_init(struct impl *impl)
{
	uint32_t flags = 0;
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
