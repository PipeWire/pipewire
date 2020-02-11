/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <unistd.h>
#include <errno.h>

#include <spa/utils/defs.h>

#include <pulse/xmalloc.h>

#define MAX_ALLOC_SIZE (1024*1024*96) /* 96MB */

static void oom(void) {
	static const char e[] = "Not enough memory\n";
	if (write(STDERR_FILENO, e, sizeof(e)-1) < 0)
		perror("write");
#ifdef SIGQUIT
	raise(SIGQUIT);
#endif
	_exit(1);
}

SPA_EXPORT
void* pa_xmalloc(size_t l)
{
	void *p;
	spa_assert(l > 0);
	spa_assert(l < MAX_ALLOC_SIZE);

	if (!(p = malloc(l)))
		oom();

	return p;
}

SPA_EXPORT
void *pa_xmalloc0(size_t l)
{
	void *p;
	spa_assert(l > 0);
	spa_assert(l < MAX_ALLOC_SIZE);

	if (!(p = calloc(1, l)))
		oom();

	return p;
}

SPA_EXPORT
void *pa_xrealloc(void *ptr, size_t size)
{
	void *p;
	spa_assert(size > 0);
	spa_assert(size < MAX_ALLOC_SIZE);

	if (!(p = realloc(ptr, size)))
		oom();
	return p;
}

SPA_EXPORT
void pa_xfree(void *p)
{
	int saved_errno;
	if (!p)
		return;
	saved_errno = errno;
	free(p);
	errno = saved_errno;
}

SPA_EXPORT
char *pa_xstrdup(const char *s)
{
	if (!s)
		return NULL;
	return pa_xmemdup(s, strlen(s)+1);
}

SPA_EXPORT
char *pa_xstrndup(const char *s, size_t l)
{
	char *e, *r;

	if (!s)
		return NULL;

	if ((e = memchr(s, 0, l)))
		return pa_xmemdup(s, (size_t) (e-s+1));

	r = pa_xmalloc(l+1);
	memcpy(r, s, l);
	r[l] = 0;
	return r;
}

SPA_EXPORT
void* pa_xmemdup(const void *p, size_t l)
{
	if (!p)
		return NULL;
	else {
		char *r = pa_xmalloc(l);
		memcpy(r, p, l);
		return r;
	}
}
