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

#include <spa/support/log.h>
#include <spa/utils/result.h>


bool dmabuf_check_sync_file_import_export(struct spa_log *log) {
	return false;
}

bool dmabuf_import_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags, int sync_file_fd) {
	spa_log_error("DMA-BUF sync_file import IOCTL not available on this system");
	return false;
}

int dmabuf_export_sync_file(struct spa_log *log, int dmabuf_fd, uint32_t flags) {
	spa_log_error("DMA-BUF sync_file export IOCTL not available on this system");
	return false;
}
