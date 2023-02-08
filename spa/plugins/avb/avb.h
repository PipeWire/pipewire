/* Spa AVB */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AVB_H
#define SPA_AVB_H

#include <spa/support/log.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT avb_log_topic
extern struct spa_log_topic *avb_log_topic;

static inline void avb_log_topic_init(struct spa_log *log)
{
	spa_log_topic_init(log, avb_log_topic);
}

#endif /* SPA_AVB_H */
