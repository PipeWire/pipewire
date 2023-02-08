/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 * We need to export open* etc., but _FORTIFY_SOURCE defines conflicting
 * always_inline versions. Disable _FORTIFY_SOURCE for this file, so we
 * can define our overrides.
 */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pipewire-v4l2.h"

#define SPA_EXPORT __attribute__((visibility("default")))

#define extract_va_arg(type, arg, last)	\
{					\
	va_list ap;			\
	va_start(ap, last);		\
	arg = va_arg(ap, type);		\
	va_end(ap);			\
}

SPA_EXPORT int open(const char *path, int oflag, ...)
{
	mode_t mode = 0;
	if (oflag & O_CREAT || oflag & O_TMPFILE)
		extract_va_arg(mode_t, mode, oflag);

	return get_fops()->openat(AT_FDCWD, path, oflag, mode);
}

/* _FORTIFY_SOURCE redirects open to __open_2 */
SPA_EXPORT int __open_2(const char *path, int oflag)
{
	return open(path, oflag);
}

#ifndef open64
SPA_EXPORT int open64(const char *path, int oflag, ...)
{
	mode_t mode = 0;
	if (oflag & O_CREAT || oflag & O_TMPFILE)
		extract_va_arg(mode_t, mode, oflag);

	return get_fops()->openat(AT_FDCWD, path, oflag | O_LARGEFILE, mode);
}

SPA_EXPORT int __open64_2(const char *path, int oflag)
{
        return open(path, oflag);
}
#endif

SPA_EXPORT int openat(int dirfd, const char *path, int oflag, ...)
{
	mode_t mode = 0;
	if (oflag & O_CREAT || oflag & O_TMPFILE)
		extract_va_arg(mode_t, mode, oflag);

	return get_fops()->openat(dirfd, path, oflag, mode);
}

SPA_EXPORT int __openat_2(int dirfd, const char *path, int oflag)
{
	return openat(dirfd, path, oflag);
}

#ifndef openat64
SPA_EXPORT int openat64(int dirfd, const char *path, int oflag, ...)
{
	mode_t mode = 0;
	if (oflag & O_CREAT || oflag & O_TMPFILE)
		extract_va_arg(mode_t, mode, oflag);

	return get_fops()->openat(dirfd, path, oflag | O_LARGEFILE, mode);
}

SPA_EXPORT int __openat64_2(int dirfd, const char *path, int oflag)
{
	return openat(dirfd, path, oflag);
}
#endif

SPA_EXPORT int dup(int oldfd)
{
	return get_fops()->dup(oldfd);
}

SPA_EXPORT int close(int fd)
{
	return get_fops()->close(fd);
}

SPA_EXPORT void *mmap(void *addr, size_t length, int prot, int flags,
                            int fd, off_t offset)
{
	return get_fops()->mmap(addr, length, prot, flags, fd, offset);
}

#ifndef mmap64
SPA_EXPORT void *mmap64(void *addr, size_t length, int prot, int flags,
                              int fd, off64_t offset)
{
	return get_fops()->mmap(addr, length, prot, flags, fd, offset);
}
#endif

SPA_EXPORT int munmap(void *addr, size_t length)
{
	return get_fops()->munmap(addr, length);
}

SPA_EXPORT int ioctl(int fd, unsigned long int request, ...)
{
	void *arg;
	extract_va_arg(void *, arg, request);
	return get_fops()->ioctl(fd, request, arg);
}
