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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

#include <pipewire/log.h>
#include <pipewire/mem.h>

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


#define USE_MEMFD

/** Map a memblock
 * \param mem a memblock
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
int pw_memblock_map(struct pw_memblock *mem)
{
	if (mem->ptr != NULL)
		return SPA_RESULT_OK;

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
				return SPA_RESULT_NO_MEMORY;

			ptr =
			    mmap(mem->ptr, mem->size, prot, MAP_FIXED | MAP_SHARED, mem->fd,
				 mem->offset);
			if (ptr != mem->ptr) {
				munmap(mem->ptr, mem->size << 1);
				return SPA_RESULT_NO_MEMORY;
			}

			ptr =
			    mmap(mem->ptr + mem->size, mem->size, prot, MAP_FIXED | MAP_SHARED,
				 mem->fd, mem->offset);
			if (ptr != mem->ptr + mem->size) {
				munmap(mem->ptr, mem->size << 1);
				return SPA_RESULT_NO_MEMORY;
			}
		} else {
			mem->ptr = mmap(NULL, mem->size, prot, MAP_SHARED, mem->fd, 0);
			if (mem->ptr == MAP_FAILED)
				return SPA_RESULT_NO_MEMORY;
		}
	} else {
		mem->ptr = NULL;
	}
	return SPA_RESULT_OK;
}

/** Create a new memblock
 * \param flags memblock flags
 * \param size size to allocate
 * \param[out] mem memblock structure to fill
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
int pw_memblock_alloc(enum pw_memblock_flags flags, size_t size, struct pw_memblock *mem)
{
	bool use_fd;

	if (mem == NULL || size == 0)
		return SPA_RESULT_INVALID_ARGUMENTS;

	mem->offset = 0;
	mem->flags = flags;
	mem->size = size;
	mem->ptr = NULL;

	use_fd = ! !(flags & (PW_MEMBLOCK_FLAG_MAP_TWICE | PW_MEMBLOCK_FLAG_WITH_FD));

	if (use_fd) {
#ifdef USE_MEMFD
		mem->fd = memfd_create("pipewire-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (mem->fd == -1) {
			pw_log_error("Failed to create memfd: %s\n", strerror(errno));
			return SPA_RESULT_ERRNO;
		}
#else
		char filename[] = "/dev/shm/pipewire-tmpfile.XXXXXX";
		mem->fd = mkostemp(filename, O_CLOEXEC);
		if (mem->fd == -1) {
			pw_log_error("Failed to create temporary file: %s\n", strerror(errno));
			return SPA_RESULT_ERRNO;
		}
		unlink(filename);
#endif

		if (ftruncate(mem->fd, size) < 0) {
			pw_log_warn("Failed to truncate temporary file: %s", strerror(errno));
			close(mem->fd);
			return SPA_RESULT_ERRNO;
		}
#ifdef USE_MEMFD
		if (flags & PW_MEMBLOCK_FLAG_SEAL) {
			unsigned int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
			if (fcntl(mem->fd, F_ADD_SEALS, seals) == -1) {
				pw_log_warn("Failed to add seals: %s", strerror(errno));
			}
		}
#endif
		if (pw_memblock_map(mem) != SPA_RESULT_OK)
			goto mmap_failed;
	} else {
		mem->ptr = malloc(size);
		if (mem->ptr == NULL)
			return SPA_RESULT_NO_MEMORY;
		mem->fd = -1;
	}
	if (!(flags & PW_MEMBLOCK_FLAG_WITH_FD) && mem->fd != -1) {
		close(mem->fd);
		mem->fd = -1;
	}
	return SPA_RESULT_OK;

      mmap_failed:
	close(mem->fd);
	return SPA_RESULT_NO_MEMORY;
}

/** Free a memblock
 * \param mem a memblock
 * \memberof pw_memblock
 */
void pw_memblock_free(struct pw_memblock *mem)
{
	if (mem == NULL)
		return;

	if (mem->flags & PW_MEMBLOCK_FLAG_WITH_FD) {
		if (mem->ptr)
			munmap(mem->ptr, mem->size);
		if (mem->fd != -1)
			close(mem->fd);
	} else {
		free(mem->ptr);
	}
	mem->ptr = NULL;
	mem->fd = -1;
}
