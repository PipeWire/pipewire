/* Spa ALSA Source */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_ALSA_H
#define SPA_ALSA_H

#include <spa/support/log.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT alsa_log_topic
extern struct spa_log_topic *alsa_log_topic;

static inline void alsa_log_topic_init(struct spa_log *log)
{
	spa_log_topic_init(log, alsa_log_topic);
}

#endif /* SPA_ALSA_H */
