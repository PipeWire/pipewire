/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/param/audio/raw.h>

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

struct volume {
	uint32_t cpu_flags;
	const char *func_name;

	struct spa_log *log;

	uint32_t flags;

	void (*process) (struct volume *vol, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, float volume, uint32_t n_samples);
	void (*free) (struct volume *vol);

	void *data;
};

int volume_init(struct volume *vol);

#define volume_process(vol,...)		(vol)->process(vol, __VA_ARGS__)
#define volume_free(vol)		(vol)->free(vol)

#define DEFINE_FUNCTION(name,arch)			\
void volume_##name##_##arch(struct volume *vol,		\
		void * SPA_RESTRICT dst,		\
		const void * SPA_RESTRICT src,		\
		float volume, uint32_t n_samples);

#define VOLUME_OPS_MAX_ALIGN	16

DEFINE_FUNCTION(f32, c);

#if defined (HAVE_SSE)
DEFINE_FUNCTION(f32, sse);
#endif

#undef DEFINE_FUNCTION
