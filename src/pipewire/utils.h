/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#ifndef PIPEWIRE_UTILS_H
#define PIPEWIRE_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>

/** \class pw_utils
 *
 * Various utility functions
 */

/** a function to destroy an item \memberof pw_utils */
typedef void (*pw_destroy_t) (void *object);

const char *
pw_split_walk(const char *str, const char *delimiter, size_t *len, const char **state);

char **
pw_split_strv(const char *str, const char *delimiter, int max_tokens, int *n_tokens);

void
pw_free_strv(char **str);

char *
pw_strip(char *str, const char *whitespace);

/** Copy a pod structure \memberof pw_utils  */
static inline struct spa_pod *
pw_spa_pod_copy(const struct spa_pod *pod)
{
	size_t size;
	struct spa_pod *c;

	size = SPA_POD_SIZE(pod);
	if ((c = (struct spa_pod *) malloc(size)) == NULL)
		return NULL;

	return (struct spa_pod *) memcpy(c, pod, size);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PIPEWIRE_UTILS_H */
