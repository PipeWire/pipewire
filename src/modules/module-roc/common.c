#include <pipewire/log.h>
#include <roc/log.h>

#include "common.h"

PW_LOG_TOPIC(roc_log_topic, "mod.roc.lib");

void pw_roc_log_init(void)
{
	roc_log_set_handler(pw_roc_log_handler, NULL);
	roc_log_set_level(pw_roc_log_level_pw_2_roc(roc_log_topic->has_custom_level ? roc_log_topic->level : pw_log_level));
}

void pw_roc_log_handler(const roc_log_message *message, void *argument)
{
	const enum spa_log_level log_level = pw_roc_log_level_roc_2_pw(message->level);
	if (SPA_UNLIKELY(pw_log_topic_enabled(log_level, roc_log_topic))) {
		pw_log_logt(log_level, roc_log_topic, message->file, message->line, message->module, message->text, "");
	}
}

