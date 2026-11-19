/* Spa sndio support */
/* SPDX-FileCopyrightText: Copyright © 2026 Antoine Jacoutot <ajacoutot@openbsd.org> */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_SNDIO_UTILS_H
#define SPA_SNDIO_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sndio.h>

#include <spa/utils/string.h>

/*
 * Device name to use when reconnecting after sndiod went away.
 * "default" is resolved the way libsndio does (the per-direction
 * variable if given, then AUDIODEVICE), but without its fallback to
 * the raw device: while sndiod restarts, "default" would open rsnd/0
 * or the raw mixer directly, keep sndiod from reclaiming the hardware
 * and never report a hangup once sndiod is back.
 */
static inline void spa_sndio_reconnect_name(const char *path,
		const char *dir_env, char *buf, size_t len)
{
	const char *s = path;

	if (s == NULL || s[0] == '\0' || spa_streq(s, SIO_DEVANY)) {
		s = dir_env != NULL ? getenv(dir_env) : NULL;
		if (s == NULL)
			s = getenv("AUDIODEVICE");
		if (s == NULL || s[0] == '\0' || spa_streq(s, SIO_DEVANY))
			s = "snd/default";
	}
	spa_scnprintf(buf, len, "%s", s);
}

/*
 * libsndio talks to sndiod with write(2): once sndiod is gone, the
 * next message it sends (e.g. the stop request in sio_close()) raises
 * SIGPIPE, whose default action kills the whole daemon.  libsndio does
 * not use MSG_NOSIGNAL and SO_NOSIGPIPE does not exist everywhere (not
 * on OpenBSD), so keep SIGPIPE blocked in every thread that calls into
 * libsndio: the write then fails with EPIPE, which libsndio reports as
 * end of file.  Done once per thread; nothing in the daemon expects
 * SIGPIPE.  Implemented in sndio.c.
 */
void spa_sndio_block_sigpipe(void);

#endif /* SPA_SNDIO_UTILS_H */
