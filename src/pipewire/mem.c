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
#include <spa/buffer/buffer.h>

#include <pipewire/log.h>
#include <pipewire/map.h>
#include <pipewire/mem.h>

#define USE_MEMFD

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

static struct spa_list _mempools = SPA_LIST_INIT(&_mempools);

#define pw_mempool_emit(p,m,v,...) spa_hook_list_call(&p->listener_list, struct pw_mempool_events, m, v, ##__VA_ARGS__)
#define pw_mempool_emit_destroy(p)	pw_mempool_emit(p, destroy, 0)
#define pw_mempool_emit_added(p,b)	pw_mempool_emit(p, added, 0, b)
#define pw_mempool_emit_removed(p,b)	pw_mempool_emit(p, removed, 0, b)

struct mempool {
	struct pw_mempool this;

	struct spa_list link;

	struct spa_hook_list listener_list;

	struct pw_map map;
	struct spa_list blocks;
	uint32_t pagesize;
};

struct memblock {
	struct pw_memblock this;
	struct spa_list link;
	struct spa_list mappings;
	struct spa_list maps;
};

struct mapping {
	struct memblock *block;
	int ref;
	uint32_t offset;
	uint32_t size;
	struct spa_list link;
	void *ptr;
};

struct memmap {
	struct pw_memmap this;
	struct mapping *mapping;
	struct spa_list link;
};

struct pw_mempool *pw_mempool_new(struct pw_properties *props)
{
	struct mempool *impl;
	struct pw_mempool *this;

	impl = calloc(1, sizeof(struct mempool));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->props = props;

	impl->pagesize = sysconf(_SC_PAGESIZE);

	pw_log_debug("mempool %p: new", this);

	spa_hook_list_init(&impl->listener_list);
	pw_map_init(&impl->map, 64, 64);
	spa_list_init(&impl->blocks);

	spa_list_append(&_mempools, &impl->link);

	return this;
}

void pw_mempool_destroy(struct pw_mempool *pool)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	pw_log_debug("mempool %p: destroy", pool);

	pw_mempool_emit_destroy(impl);

	spa_list_remove(&impl->link);

	spa_list_consume(b, &impl->blocks, link)
		pw_memblock_free(&b->this);

	if (pool->props)
		pw_properties_free(pool->props);
	free(impl);
}


void pw_mempool_add_listener(struct pw_mempool *pool,
			     struct spa_hook *listener,
			     const struct pw_mempool_events *events,
			     void *data)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	spa_hook_list_append(&impl->listener_list, listener, events, data);
}

#if 0
/** Map a memblock
 * \param mem a memblock
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
SPA_EXPORT
int pw_memblock_map_old(struct pw_memblock *mem)
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
			void *ptr, *wrap;

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

			wrap = SPA_MEMBER(mem->ptr, mem->size, void);

			ptr =
			    mmap(wrap, mem->size, prot, MAP_FIXED | MAP_SHARED,
				 mem->fd, mem->offset);
			if (ptr != wrap) {
				munmap(mem->ptr, mem->size << 1);
				return -ENOMEM;
			}
		} else {
			mem->ptr = mmap(NULL, mem->size, prot, MAP_SHARED, mem->fd, 0);
			if (mem->ptr == MAP_FAILED)
				return -errno;
		}
	} else {
		mem->ptr = NULL;
	}

	pw_log_debug("mem %p: map to %p", mem, mem->ptr);

	return 0;
}
#endif

static struct mapping * memblock_find_mapping(struct memblock *b,
		uint32_t flags, uint32_t offset, uint32_t size)
{
	struct mapping *m;

	spa_list_for_each(m, &b->mappings, link) {
		if (m->offset <= offset && (m->offset + m->size) >= (offset + size))
			return m;
	}
	return NULL;
}

static struct mapping * memblock_map(struct memblock *b,
		enum pw_memmap_flags flags, uint32_t offset, uint32_t size)
{
	struct mempool *p = SPA_CONTAINER_OF(b->this.pool, struct mempool, this);
	struct mapping *m;
	void *ptr;
	int prot = 0;

	if (flags & PW_MEMMAP_FLAG_READ)
		prot |= PROT_READ;
	if (flags & PW_MEMMAP_FLAG_WRITE)
		prot |= PROT_WRITE;

	if (flags & PW_MEMMAP_FLAG_TWICE) {
		errno = -ENOTSUP;
		return NULL;
	}

	ptr = mmap(NULL, size, prot, MAP_SHARED, b->this.fd, offset);
	if (ptr == MAP_FAILED) {
		pw_log_error("pool %p: Failed to mmap memory %d size:%d: %m",
				p, b->this.fd, size);
		return NULL;
	}

	m = calloc(1, sizeof(struct mapping));
	if (m == NULL) {
		munmap(ptr, size);
		return NULL;
	}
	m->ptr = ptr;
	m->block = b;
	m->offset = offset;
	m->size = size;
	b->this.ref++;
	spa_list_append(&b->mappings, &m->link);

        pw_log_debug("pool %p: fd:%d map:%p ptr:%p (%d %d)", p,
			b->this.fd, m, m->ptr, offset, size);

	return m;
}

static void mapping_unmap(struct mapping *m)
{
	struct memblock *b = m->block;
	struct mempool *p = SPA_CONTAINER_OF(b->this.pool, struct mempool, this);

        pw_log_debug("pool %p: map:%p fd:%d ptr:%p size:%d", p, m, b->this.fd, m->ptr, m->size);

	munmap(m->ptr, m->size);
	spa_list_remove(&m->link);
	free(m);

	pw_memblock_unref(&b->this);
}

SPA_EXPORT
struct pw_memmap * pw_memblock_map(struct pw_memblock *block,
		enum pw_memmap_flags flags, uint32_t offset, uint32_t size)
{
	struct memblock *b = SPA_CONTAINER_OF(block, struct memblock, this);
	struct mempool *p = SPA_CONTAINER_OF(block->pool, struct mempool, this);
	struct mapping *m;
	struct memmap *mm;
	struct pw_map_range range;

	pw_map_range_init(&range, offset, size, p->pagesize);

	m = memblock_find_mapping(b, flags, range.offset, range.size);
	if (m == NULL)
		m = memblock_map(b, flags, range.offset, range.size);
	if (m == NULL)
		return NULL;

	mm = calloc(1, sizeof(struct memmap));
	if (mm == NULL) {
		if (m->ref == 0)
			mapping_unmap(m);
		return NULL;
	}

	m->ref++;
	mm->mapping = m;
	mm->this.block = block;
	mm->this.flags = flags;
	mm->this.offset = offset;
	mm->this.size = size;
	mm->this.ptr = SPA_MEMBER(m->ptr, range.start, void);

	spa_list_append(&b->maps, &mm->link);

        pw_log_debug("pool %p: map:%p fd:%d ptr:%p (%d %d) mapping:%p ref:%d", p,
			&mm->this, b->this.fd, mm->this.ptr, offset, size, m, m->ref);

	return &mm->this;
}

SPA_EXPORT
struct pw_memmap * pw_mempool_map_id(struct pw_mempool *pool,
		uint32_t id, enum pw_memmap_flags flags, uint32_t offset, uint32_t size)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	b = pw_map_lookup(&impl->map, id);
	if (b == NULL) {
		errno = -ENOENT;
		return NULL;
	}
	return pw_memblock_map(&b->this, flags, offset, size);
}

SPA_EXPORT
int pw_memmap_free(struct pw_memmap *map)
{
	struct memmap *mm = SPA_CONTAINER_OF(map, struct memmap, this);
	struct mapping *m = mm->mapping;
	struct memblock *b = m->block;
	struct mempool *p = SPA_CONTAINER_OF(b->this.pool, struct mempool, this);

        pw_log_debug("pool %p: map:%p fd:%d ptr:%p map:%p ref:%d", p,
			&mm->this, b->this.fd, mm->this.ptr, m, m->ref);

	if (--m->ref == 0)
		mapping_unmap(m);

	spa_list_remove(&mm->link);
	free(mm);

	return 0;
}

/** Create a new memblock
 * \param flags memblock flags
 * \param size size to allocate
 * \param[out] mem memblock structure to fill
 * \return 0 on success, < 0 on error
 * \memberof pw_memblock
 */
SPA_EXPORT
int pw_mempool_alloc(struct pw_mempool *pool, enum pw_memblock_flags flags,
		size_t size, struct pw_memblock **mem)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;
	int res;

	spa_return_val_if_fail(pool != NULL, -EINVAL);
	spa_return_val_if_fail(mem != NULL, -EINVAL);

	b = calloc(1, sizeof(struct memblock));
	if (b == NULL)
		return -errno;

	b->this.ref = 1;
	b->this.pool = pool;
	b->this.flags = flags;
	spa_list_init(&b->mappings);
	spa_list_init(&b->maps);

#ifdef USE_MEMFD
	b->this.fd = memfd_create("pipewire-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (b->this.fd == -1) {
		res = -errno;
		pw_log_error("Failed to create memfd: %s\n", strerror(errno));
		goto error_free;
	}
#else
	char filename[] = "/dev/shm/pipewire-tmpfile.XXXXXX";
	b->this.fd = mkostemp(filename, O_CLOEXEC);
	if (b->this.fd == -1) {
		res = -errno;
		pw_log_error("Failed to create temporary file: %s\n", strerror(errno));
		goto error_free;
	}
	unlink(filename);
#endif

	if (ftruncate(b->this.fd, size) < 0) {
		res = -errno;
		pw_log_warn("Failed to truncate temporary file: %s", strerror(errno));
		goto error_close;
	}
#ifdef USE_MEMFD
	if (flags & PW_MEMBLOCK_FLAG_SEAL) {
		unsigned int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
		if (fcntl(b->this.fd, F_ADD_SEALS, seals) == -1) {
			pw_log_warn("Failed to add seals: %s", strerror(errno));
		}
	}
#endif
	b->this.type = SPA_DATA_MemFd;

	if (flags & PW_MEMBLOCK_FLAG_MAP && size > 0) {
		b->this.map = pw_memblock_map(&b->this, PROT_READ|PROT_WRITE, 0, size);
		if (b->this.map == NULL) {
			res = -errno;
			goto error_close;
		}
	}

	b->this.id = pw_map_insert_new(&impl->map, b);
	spa_list_append(&impl->blocks, &b->link);
	pw_log_debug("mem %p: alloc id:%d", &b->this, b->this.id);

	pw_mempool_emit_added(impl, &b->this);
	*mem = &b->this;

	return 0;

error_close:
	close(b->this.fd);
error_free:
	free(b);
	return res;
}

static struct memblock * mempool_find_fd(struct pw_mempool *pool, int fd)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	spa_list_for_each(b, &impl->blocks, link) {
		if (fd == b->this.fd) {
			pw_log_debug("pool %p: found %p id:%d for fd %d", pool, &b->this, b->this.id, fd);
			return b;
		}
	}
	return NULL;
}

SPA_EXPORT
struct pw_memblock * pw_mempool_import(struct pw_mempool *pool,
		uint32_t type, int fd, uint32_t flags)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	b = mempool_find_fd(pool, fd);
	if (b != NULL) {
		b->this.ref++;
		return &b->this;
	}

	b = calloc(1, sizeof(struct memblock));
	if (b == NULL)
		return NULL;

	spa_list_init(&b->maps);
	spa_list_init(&b->mappings);

	b->this.ref = 1;
	b->this.pool = pool;
	b->this.type = type;
	b->this.fd = fd;
	b->this.flags = flags;
	b->this.id = pw_map_insert_new(&impl->map, b);
	spa_list_append(&impl->blocks, &b->link);

	pw_log_debug("pool %p: import %p id:%u fd:%d", pool, b, b->this.id, fd);

	pw_mempool_emit_added(impl, &b->this);

	return &b->this;
}

SPA_EXPORT
struct pw_memblock * pw_mempool_import_block(struct pw_mempool *pool,
		struct pw_memblock *mem)
{
	return pw_mempool_import(pool, mem->type, mem->fd,
			mem->flags | PW_MEMBLOCK_FLAG_DONT_CLOSE);
}


int pw_mempool_remove_id(struct pw_mempool *pool, uint32_t id)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	b = pw_map_lookup(&impl->map, id);
	if (b == NULL)
		return -ENOENT;

	pw_memblock_unref(&b->this);
	return 0;
}

/** Free a memblock
 * \param mem a memblock
 * \memberof pw_memblock
 */
SPA_EXPORT
void pw_memblock_free(struct pw_memblock *block)
{
	struct memblock *b = SPA_CONTAINER_OF(block, struct memblock, this);
	struct pw_mempool *pool = block->pool;
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memmap *mm;

	spa_return_if_fail(block != NULL);

	pw_log_debug("pool %p: free mem %p id:%d fd:%d ref:%d",
			pool, block, block->id, block->fd, block->ref);

	block->ref++;

	pw_map_remove(&impl->map, block->id);
	spa_list_remove(&b->link);

	pw_mempool_emit_removed(impl, block);

	spa_list_consume(mm, &b->maps, link)
		pw_memmap_free(&mm->this);

	if (block->fd != -1 && !(block->flags & PW_MEMBLOCK_FLAG_DONT_CLOSE)) {
		pw_log_debug("pool %p: close fd:%d", pool, block->fd);
		close(block->fd);
	}
	free(b);
}

SPA_EXPORT
struct pw_memblock * pw_mempool_find_ptr(struct pw_mempool *pool, const void *ptr)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;
	struct mapping *m;

	spa_list_for_each(b, &impl->blocks, link) {
		spa_list_for_each(m, &b->mappings, link) {
			if (ptr >= m->ptr && ptr < SPA_MEMBER(m->ptr, m->size, void)) {
				pw_log_debug("pool %p: found %p id:%d for %p", pool,
						m->block, b->this.id, ptr);
				return &b->this;
			}
		}
	}
	return NULL;
}

SPA_EXPORT
struct pw_memblock * pw_mempool_find_id(struct pw_mempool *pool, uint32_t id)
{
	struct mempool *impl = SPA_CONTAINER_OF(pool, struct mempool, this);
	struct memblock *b;

	b = pw_map_lookup(&impl->map, id);
	pw_log_debug("pool %p: found %p for %d", pool, b, id);
	if (b == NULL)
		return NULL;

	return &b->this;
}

SPA_EXPORT
struct pw_memblock * pw_mempool_find_fd(struct pw_mempool *pool, int fd)
{
	struct memblock *b;

	b = mempool_find_fd(pool, fd);
	if (b == NULL)
		return NULL;

	return &b->this;
}
