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

#ifndef RENDER_DMABUF_H
#define RENDER_DMABUF_H

#include <stdbool.h>
#include <stdint.h>

#include "spa/support/log.h"

// Copied from <linux/dma-buf.h> to avoid #ifdef soup
#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)

/**
 * Check whether DMA-BUF import/export from/to sync_file is available.
 *
 * If this function returns true, dmabuf_import_sync_file() is supported.
 */
bool dmabuf_check_sync_file_import_export(struct spa_log *log);

/**
 * Import a sync_file into a DMA-BUF with DMA_BUF_IOCTL_IMPORT_SYNC_FILE.
 *
 * This can be used to make explicit sync interoperate with implicit sync.
 */
bool dmabuf_import_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags, int sync_file_fd);

/**
 * Export a sync_file from a DMA-BUF with DMA_BUF_IOCTL_EXPORT_SYNC_FILE.
 *
 * The sync_file FD is returned on success, -1 is returned on error.
 *
 * This can be used to make explicit sync interoperate with implicit sync.
 */
int dmabuf_export_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags);

#endif
