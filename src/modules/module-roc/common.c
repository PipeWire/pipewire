#include <pipewire/log.h>
#include <roc/log.h>

#include "common.h"

PW_LOG_TOPIC(roc_log_topic, "mod.roc.lib");

static inline roc_log_level pw_roc_log_level_pw_2_roc(const enum spa_log_level pw_log_level)
{
	switch (pw_log_level) {
	case SPA_LOG_LEVEL_NONE:
		return ROC_LOG_NONE;
	case SPA_LOG_LEVEL_ERROR:
		return ROC_LOG_ERROR;
	case SPA_LOG_LEVEL_WARN:
		return ROC_LOG_ERROR;
	case SPA_LOG_LEVEL_INFO:
		return ROC_LOG_INFO;
	case SPA_LOG_LEVEL_DEBUG:
		return ROC_LOG_DEBUG;
	case SPA_LOG_LEVEL_TRACE:
		return ROC_LOG_TRACE;
	default:
		return ROC_LOG_NONE;
	}
}

static inline enum spa_log_level pw_roc_log_level_roc_2_pw(const roc_log_level roc_log_level)
{
	switch (roc_log_level) {
	case ROC_LOG_NONE:
		return SPA_LOG_LEVEL_NONE;
	case ROC_LOG_ERROR:
		return SPA_LOG_LEVEL_ERROR;
	case ROC_LOG_INFO:
		return SPA_LOG_LEVEL_INFO;
	case ROC_LOG_DEBUG:
		return SPA_LOG_LEVEL_DEBUG;
	case ROC_LOG_TRACE:
		return SPA_LOG_LEVEL_TRACE;
	default:
		return SPA_LOG_LEVEL_NONE;
	}
}

static void pw_roc_log_handler(const roc_log_message *message, void *argument)
{
	const enum spa_log_level log_level = pw_roc_log_level_roc_2_pw(message->level);
	if (SPA_UNLIKELY(pw_log_topic_enabled(log_level, roc_log_topic))) {
		pw_log_logt(log_level, roc_log_topic, message->file, message->line, message->module, message->text, "");
	}
}

void pw_roc_log_init(void)
{
	roc_log_set_handler(pw_roc_log_handler, NULL);
	roc_log_set_level(pw_roc_log_level_pw_2_roc(roc_log_topic->has_custom_level ? roc_log_topic->level : pw_log_level));
}
