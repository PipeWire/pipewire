/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

#include <spa/utils/list.h>

#include <pipewire/log.h>
#include <pipewire/mem.h>

#ifndef HAVE_MEMFD_CREATE
/*
 * No glibc wrappers exist for memfd_create(2), so provide our own.
 *
 * Also define memfd fcntl sealing macros. While they are already
 * defined in the kernel header file <linux/fcntl.h>, that file as
 * a whole conflicts with the original glibc header <fnctl.h>.
 */

static inline int memfd_create(const char *name, unsigned int flags)
{
	return syscall(SYS_memfd_create, name, flags);
}
#endif

/* memfd_create(2) flags */

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC       0x0001U
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

/* fcntl() seals-related flags */

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif

#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)

#define F_SEAL_SEAL     0x0001	/* prevent further seals from being set */
#define F_SEAL_SHRINK   0x0002	/* prevent file from shrinking */
#define F_SEAL_GROW     0x0004	/* prevent file from growing */
#define F_SEAL_WRITE    0x0008	/* prevent writes */
#endif

struct memblock {
	struct pw_memblock mem;
	struct spa_list link;
};

static struct spa_list _memblocks = SPA_LIST_INIT(&_memblocks);

#define USE_MEMFD

/** Map a memblock
 * \param mem a memblock
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
int pw_memblock_map(struct pw_memblock *mem)
{
	if (mem->ptr != NULL)
		return 0;

	if (mem->flags & PW_MEMBLOCK_FLAG_MAP_READWRITE) {
		int prot = 0;

		if (mem->flags & PW_MEMBLOCK_FLAG_MAP_READ)
			prot |= PROT_READ;
		if (mem->flags & PW_MEMBLOCK_FLAG_MAP_WRITE)
			prot |= PROT_WRITE;

		if (mem->flags & PW_MEMBLOCK_FLAG_MAP_TWICE) {
			void *ptr;

			mem->ptr =
			    mmap(NULL, mem->size << 1, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1,
				 0);
			if (mem->ptr == MAP_FAILED)
				return -errno;

			ptr =
			    mmap(mem->ptr, mem->size, prot, MAP_FIXED | MAP_SHARED, mem->fd,
				 mem->offset);
			if (ptr != mem->ptr) {
				munmap(mem->ptr, mem->size << 1);
				return -ENOMEM;
			}

			ptr =
			    mmap(mem->ptr + mem->size, mem->size, prot, MAP_FIXED | MAP_SHARED,
				 mem->fd, mem->offset);
			if (ptr != mem->ptr + mem->size) {
				munmap(mem->ptr, mem->size << 1);
				return -ENOMEM;
			}
		} else {
			mem->ptr = mmap(NULL, mem->size, prot, MAP_SHARED, mem->fd, 0);
			if (mem->ptr == MAP_FAILED)
				return -ENOMEM;
		}
	} else {
		mem->ptr = NULL;
	}

	pw_log_debug("mem %p: map to %p", mem, mem->ptr);

	return 0;
}

/** Create a new memblock
 * \param flags memblock flags
 * \param size size to allocate
 * \param[out] mem memblock structure to fill
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
int pw_memblock_alloc(enum pw_memblock_flags flags, size_t size, struct pw_memblock **mem)
{
	struct memblock tmp, *p;
	struct pw_memblock *m;
	bool use_fd;

	if (mem == NULL)
		return -EINVAL;

	m = &tmp.mem;
	m->offset = 0;
	m->flags = flags;
	m->size = size;
	m->ptr = NULL;

	use_fd = ! !(flags & (PW_MEMBLOCK_FLAG_MAP_TWICE | PW_MEMBLOCK_FLAG_WITH_FD));

	if (use_fd) {
#ifdef USE_MEMFD
		m->fd = memfd_create("pipewire-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (m->fd == -1) {
			pw_log_error("Failed to create memfd: %s\n", strerror(errno));
			return -errno;
		}
#else
		char filename[] = "/dev/shm/pipewire-tmpfile.XXXXXX";
		m->fd = mkostemp(filename, O_CLOEXEC);
		if (m->fd == -1) {
			pw_log_error("Failed to create temporary file: %s\n", strerror(errno));
			return -errno;
		}
		unlink(filename);
#endif

		if (ftruncate(m->fd, size) < 0) {
			pw_log_warn("Failed to truncate temporary file: %s", strerror(errno));
			close(m->fd);
			return -errno;
		}
#ifdef USE_MEMFD
		if (flags & PW_MEMBLOCK_FLAG_SEAL) {
			unsigned int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
			if (fcntl(m->fd, F_ADD_SEALS, seals) == -1) {
				pw_log_warn("Failed to add seals: %s", strerror(errno));
			}
		}
#endif
		if (pw_memblock_map(m) != 0)
			goto mmap_failed;
	} else {
		if (size > 0) {
			m->ptr = malloc(size);
			if (m->ptr == NULL)
				return -ENOMEM;
		}
		m->fd = -1;
	}
	if (!(flags & PW_MEMBLOCK_FLAG_WITH_FD) && m->fd != -1) {
		close(m->fd);
		m->fd = -1;
	}

	p = calloc(1, sizeof(struct memblock));
	*p = tmp;
	spa_list_append(&_memblocks, &p->link);
	*mem = &p->mem;
	pw_log_debug("mem %p: alloc", *mem);

	return 0;

      mmap_failed:
	close(m->fd);
	return -ENOMEM;
}

int
pw_memblock_import(enum pw_memblock_flags flags,
		   int fd, off_t offset, size_t size,
		   struct pw_memblock **mem)
{
	int res;

	if ((res = pw_memblock_alloc(0, 0, mem)) < 0)
		return res;

	(*mem)->flags = flags;
	(*mem)->fd = fd;
	(*mem)->offset = offset;
	(*mem)->size = size;

	pw_log_debug("mem %p: import", *mem);

	return pw_memblock_map(*mem);
}

/** Free a memblock
 * \param mem a memblock
 * \memberof pw_memblock
 */
void pw_memblock_free(struct pw_memblock *mem)
{
	struct memblock *m = (struct memblock *)mem;

	if (mem == NULL)
		return;

	pw_log_debug("mem %p: free", mem);
	if (mem->flags & PW_MEMBLOCK_FLAG_WITH_FD) {
		if (mem->ptr)
			munmap(mem->ptr, mem->size);
		if (mem->fd != -1)
			close(mem->fd);
	} else {
		free(mem->ptr);
	}
	spa_list_remove(&m->link);
	free(mem);
}

struct pw_memblock * pw_memblock_find(const void *ptr)
{
	struct memblock *m;

	spa_list_for_each(m, &_memblocks, link) {
		if (ptr >= m->mem.ptr && ptr < m->mem.ptr + m->mem.size)
			return &m->mem;
	}
	return NULL;
}
