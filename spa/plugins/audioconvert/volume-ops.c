/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/param/audio/format-utils.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/defs.h>

#include "volume-ops.h"

typedef void (*volume_func_t) (struct volume *vol, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, float volume, uint32_t n_samples);

#define MAKE(func,...) \
	{ func, #func , __VA_ARGS__ }

static const struct volume_info {
	volume_func_t process;
	const char *name;
	uint32_t cpu_flags;
} volume_table[] =
{
#if defined (HAVE_SSE)
	MAKE(volume_f32_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(volume_f32_c),
};
#undef MAKE

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct volume_info *find_volume_info(uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(volume_table, t) {
		if (MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_volume_free(struct volume *vol)
{
	vol->process = NULL;
}

int volume_init(struct volume *vol)
{
	const struct volume_info *info;

	info = find_volume_info(vol->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	vol->cpu_flags = info->cpu_flags;
	vol->func_name = info->name;
	vol->free = impl_volume_free;
	vol->process = info->process;
	return 0;
}
