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

#ifndef FLATPAK_UTILS_H
#define FLATPAK_UTILS_H

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <spa/include/spa/utils/result.h>
#include <pipewire/log.h>


static int pw_check_flatpak(pid_t pid)
{
#if defined(__linux__)
	char root_path[2048];
	int root_fd, info_fd, res;
	struct stat stat_buf;

	snprintf(root_path, sizeof(root_path), "/proc/%d/root", (int)pid);
	root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (root_fd == -1) {
		res = -errno;
		if (res == -EACCES) {
			struct statfs buf;
			/* Access to the root dir isn't allowed. This can happen if the root is on a fuse
			 * filesystem, such as in a toolbox container. We will never have a fuse rootfs
			 * in the flatpak case, so in that case its safe to ignore this and
			 * continue to detect other types of apps. */
			if (statfs(root_path, &buf) == 0 &&
			    buf.f_type == 0x65735546) /* FUSE_SUPER_MAGIC */
				return 0;
		}
		/* Not able to open the root dir shouldn't happen. Probably the app died and
		 * we're failing due to /proc/$pid not existing. In that case fail instead
		 * of treating this as privileged. */
		pw_log_info("failed to open \"%s\": %s", root_path, spa_strerror(res));
		return res;
	}
	info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
	close (root_fd);
	if (info_fd == -1) {
		if (errno == ENOENT) {
			pw_log_debug("no .flatpak-info, client on the host");
			/* No file => on the host */
			return 0;
		}
		res = -errno;
		pw_log_error("error opening .flatpak-info: %m");
		return res;
        }
	if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode)) {
		/* Some weird fd => failure, assume sandboxed */
		pw_log_error("error fstat .flatpak-info: %m");
	}
	close(info_fd);
	return 1;
#else
	return 0;
#endif
}

#endif /* FLATPAK_UTILS_H */
