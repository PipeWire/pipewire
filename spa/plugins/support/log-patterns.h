#ifndef LOG_PATTERNS_H
#define LOG_PATTERNS_H

#include <spa/support/log.h>

struct spa_list;

void support_log_topic_init(struct spa_list *patterns, enum spa_log_level default_level,
			    struct spa_log_topic *t);
int support_log_parse_patterns(struct spa_list *patterns, const char *jsonstr);
void support_log_free_patterns(struct spa_list *patterns);

#endif /* LOG_PATTERNS_H */
