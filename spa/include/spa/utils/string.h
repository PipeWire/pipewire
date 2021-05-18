/* Simple Plugin API
 *
 * Copyright © 2021 Red Hat, Inc.
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

#ifndef SPA_UTILS_STRING_H
#define SPA_UTILS_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <errno.h>

#include <spa/utils/defs.h>

/**
 * \return true if the two strings are equal, false otherwise
 *
 * If both \a a and \a b are NULL, the two are considered equal.
 */
static inline bool spa_streq(const char *s1, const char *s2)
{
	return SPA_LIKELY(s1 && s2) ? strcmp(s1, s2) == 0 : s1 == s2;
}

/**
 * \return true if the two strings are equal, false otherwise
 *
 * If both \a a and \a b are NULL, the two are considered equal.
 */
static inline bool spa_strneq(const char *s1, const char *s2, size_t len)
{
	return SPA_LIKELY(s1 && s2) ? strncmp(s1, s2, len) == 0 : s1 == s2;
}

/**
 * Convert \a str to an int32_t with the given \a base and store the
 * result in \a val.
 *
 * On failure, the value of \a val is undefined.
 *
 * \return true on success, false otherwise
 */
static inline bool spa_atoi32(const char *str, int32_t *val, int base)
{
	char *endptr;
	long v;

	if (!str || *str =='\0')
		return false;

	errno = 0;
	v = strtol(str, &endptr, base);
	if (errno != 0 || *endptr != '\0')
		return false;

	if (v != (int32_t)v)
		return false;

	*val = v;
	return true;
}

/**
 * Convert \a str to a boolean. Allowed boolean values are "true" and a
 * literal "1", anything else is false.
 *
 * \return true on success, false otherwise
 */
static inline bool spa_atob(const char *str)
{
	return spa_streq(str, "true") || spa_streq(str, "1");
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_UTILS_STRING_H */
