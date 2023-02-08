/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>

struct fops {
	int (*openat)(int dirfd, const char *path, int oflag, mode_t mode);
	int (*dup)(int oldfd);
	int (*close)(int fd);
	int (*ioctl)(int fd, unsigned long request, void *arg);
	void *(*mmap)(void *addr, size_t length, int prot,
			int flags, int fd, off64_t offset);
	int (*munmap)(void *addr, size_t length);
};

const struct fops *get_fops(void);
