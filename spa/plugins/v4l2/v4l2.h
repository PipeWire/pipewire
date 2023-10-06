/* Spa V4l2 support */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <linux/videodev2.h>

#include <spa/support/log.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &v4l2_log_topic
extern struct spa_log_topic v4l2_log_topic;

static inline void v4l2_log_topic_init(struct spa_log *log)
{
	spa_log_topic_init(log, &v4l2_log_topic);
}

struct spa_v4l2_device {
	struct spa_log *log;
	int fd;
	struct v4l2_capability cap;
	unsigned int active:1;
	unsigned int have_format:1;
	char path[64];
};

int spa_v4l2_open(struct spa_v4l2_device *dev, const char *path);
int spa_v4l2_close(struct spa_v4l2_device *dev);
int spa_v4l2_is_capture(struct spa_v4l2_device *dev);
