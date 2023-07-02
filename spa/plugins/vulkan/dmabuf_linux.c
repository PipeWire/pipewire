// Copyright (c) 2023 The wlroots contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Obtained from https://gitlab.freedesktop.org/wlroots/wlroots/

/* SPDX-FileCopyrightText: Copyright © 2023 The wlroots contributors */
/* SPDX-License-Identifier: MIT */

#include <linux/dma-buf.h>
#include <linux/version.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <xf86drm.h>
#include <errno.h>

#include <spa/support/log.h>
#include <spa/utils/result.h>

#include "dmabuf.h"

bool dmabuf_check_sync_file_import_export(struct spa_log *log) {
	/* Unfortunately there's no better way to check the availability of the
	 * IOCTL than to check the kernel version. See the discussion at:
	 * https://lore.kernel.org/dri-devel/20220601161303.64797-1-contact@emersion.fr/
	 */

	struct utsname utsname = {0};
	if (uname(&utsname) != 0) {
		spa_log_warn(log, "uname failed");
		return false;
	}

	if (strcmp(utsname.sysname, "Linux") != 0) {
		return false;
	}

	// Trim release suffix if any, e.g. "-arch1-1"
	for (size_t i = 0; utsname.release[i] != '\0'; i++) {
		char ch = utsname.release[i];
		if ((ch < '0' || ch > '9') && ch != '.') {
			utsname.release[i] = '\0';
			break;
		}
	}

	char *rel = strtok(utsname.release, ".");
	int major = atoi(rel);

	int minor = 0;
	rel = strtok(NULL, ".");
	if (rel != NULL) {
		minor = atoi(rel);
	}

	int patch = 0;
	rel = strtok(NULL, ".");
	if (rel != NULL) {
		patch = atoi(rel);
	}

	return KERNEL_VERSION(major, minor, patch) >= KERNEL_VERSION(5, 20, 0);
}

// TODO: drop these definitions once widespread

#if !defined(DMA_BUF_IOCTL_IMPORT_SYNC_FILE)

struct dma_buf_import_sync_file {
	__u32 flags;
	__s32 fd;
};

#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file)

#endif

#if !defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)

struct dma_buf_export_sync_file {
	__u32 flags;
	__s32 fd;
};

#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)

#endif

bool dmabuf_import_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags, int sync_file_fd) {
	struct dma_buf_import_sync_file data = {
		.flags = flags,
		.fd = sync_file_fd,
	};
	if (drmIoctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) != 0) {
		spa_log_error(log, "drmIoctl(IMPORT_SYNC_FILE) failed with %d (%s)", errno, spa_strerror(-errno));
		return false;
	}
	return true;
}

int dmabuf_export_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags) {
	struct dma_buf_export_sync_file data = {
		.flags = flags,
		.fd = -1,
	};
	if (drmIoctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data) != 0) {
		spa_log_error(log, "drmIoctl(EXPORT_SYNC_FILE) failed with %d (%s)", errno, spa_strerror(-errno));
		return -1;
	}
	return data.fd;
}
