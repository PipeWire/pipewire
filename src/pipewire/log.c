/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <fnmatch.h>
#include <pthread.h>

#include <spa/support/log-impl.h>

#include <spa/pod/pod.h>
#include <spa/debug/types.h>
#include <spa/debug/format.h>
#include <spa/pod/iter.h>
#include <spa/utils/list.h>
#include <spa/utils/string.h>

#include <pipewire/log.h>
#include <pipewire/private.h>

SPA_LOG_IMPL(default_log);

SPA_EXPORT
enum spa_log_level pw_log_level = DEFAULT_LOG_LEVEL;

static struct spa_log *global_log = &default_log.log;

SPA_EXPORT
struct spa_log_topic * const PW_LOG_TOPIC_DEFAULT;

struct topic {
	struct spa_list link;
	struct spa_log_topic *t;
	unsigned int refcnt;
};

struct pattern {
	struct spa_list link;
	enum spa_log_level level;
	char pattern[];
};

static struct spa_list topics = SPA_LIST_INIT(&topics);
static struct spa_list patterns = SPA_LIST_INIT(&patterns);

static pthread_mutex_t topics_lock = PTHREAD_MUTEX_INITIALIZER;

PW_LOG_TOPIC(log_buffers, "pw.buffers");
PW_LOG_TOPIC(log_client, "pw.client");
PW_LOG_TOPIC(log_conf, "pw.conf");
PW_LOG_TOPIC(log_context, "pw.context");
PW_LOG_TOPIC(log_core, "pw.core");
PW_LOG_TOPIC(log_data_loop, "pw.data-loop");
PW_LOG_TOPIC(log_device, "pw.device");
PW_LOG_TOPIC(log_factory, "pw.factory");
PW_LOG_TOPIC(log_filter, "pw.filter");
PW_LOG_TOPIC(log_global, "pw.global");
PW_LOG_TOPIC(log_link, "pw.link");
PW_LOG_TOPIC(log_loop, "pw.loop");
PW_LOG_TOPIC(log_main_loop, "pw.main-loop");
PW_LOG_TOPIC(log_mem, "pw.mem");
PW_LOG_TOPIC(log_metadata, "pw.metadata");
PW_LOG_TOPIC(log_module, "pw.module");
PW_LOG_TOPIC(log_node, "pw.node");
PW_LOG_TOPIC(log_port, "pw.port");
PW_LOG_TOPIC(log_properties, "pw.props");
PW_LOG_TOPIC(log_protocol, "pw.protocol");
PW_LOG_TOPIC(log_proxy, "pw.proxy");
PW_LOG_TOPIC(log_resource, "pw.resource");
PW_LOG_TOPIC(log_stream, "pw.stream");
PW_LOG_TOPIC(log_thread_loop, "pw.thread-loop");
PW_LOG_TOPIC(log_timer_queue, "pw.timer-queue");
PW_LOG_TOPIC(log_work_queue, "pw.work-queue");

PW_LOG_TOPIC(PW_LOG_TOPIC_DEFAULT, "default");


static struct topic *find_topic(struct spa_log_topic *t)
{
	struct topic *topic;

	spa_list_for_each(topic, &topics, link)
		if (topic->t == t)
			return topic;

	return NULL;
}

static struct topic *add_topic(struct spa_log_topic *t)
{
	struct topic *topic;

	topic = calloc(1, sizeof(struct topic));
	if (!topic)
		return NULL;

	topic->t = t;
	spa_list_append(&topics, &topic->link);
	return topic;
}

static void update_topic_level(struct spa_log_topic *t)
{
	enum spa_log_level level = pw_log_level;
	bool has_custom_level = false;
	const char *topic = t->topic;
	struct pattern *pattern;

	spa_list_for_each(pattern, &patterns, link) {
		if (fnmatch(pattern->pattern, topic, 0) != 0)
			continue;

		level = pattern->level;
		has_custom_level = true;
		break;
	}

	t->level = level;
	t->has_custom_level = has_custom_level;
}

static void update_all_topic_levels(void)
{
	struct topic *topic;

	pthread_mutex_lock(&topics_lock);

	spa_list_for_each(topic, &topics, link)
		update_topic_level(topic->t);

	pthread_mutex_unlock(&topics_lock);
}

SPA_EXPORT
void pw_log_topic_register(struct spa_log_topic *t)
{
	struct topic *topic;

	pthread_mutex_lock(&topics_lock);

	topic = find_topic(t);
	if (!topic) {
		update_topic_level(t);
		topic = add_topic(t);
		if (!topic)
			goto done;
	}

	++topic->refcnt;

done:
	pthread_mutex_unlock(&topics_lock);
}

SPA_EXPORT
void pw_log_topic_unregister(struct spa_log_topic *t)
{
	struct topic *topic;

	pthread_mutex_lock(&topics_lock);

	topic = find_topic(t);
	if (!topic)
		goto done;

	if (topic->refcnt-- <= 1) {
		spa_list_remove(&topic->link);
		free(topic);
	}

done:
	pthread_mutex_unlock(&topics_lock);
}

void pw_log_topic_register_enum(const struct spa_log_topic_enum *e)
{
	struct spa_log_topic * const *t;

	if (!e)
		return;

	for (t = e->topics; t < e->topics_end; ++t)
		pw_log_topic_register(*t);
}

void pw_log_topic_unregister_enum(const struct spa_log_topic_enum *e)
{
	struct spa_log_topic * const *t;

	if (!e)
		return;

	for (t = e->topics; t < e->topics_end; ++t)
		pw_log_topic_unregister(*t);
}

/** Set the global log interface
 * \param log the global log to set
 */
SPA_EXPORT
void pw_log_set(struct spa_log *log)
{
	global_log = log ? log : &default_log.log;
	global_log->level = pw_log_level;
}

bool pw_log_is_default(void)
{
	return global_log == &default_log.log;
}

/** Get the global log interface
 * \return the global log
 */
SPA_EXPORT
struct spa_log *pw_log_get(void)
{
	return global_log;
}

/** Set the global log level
 * \param level the new log level
 */
SPA_EXPORT
void pw_log_set_level(enum spa_log_level level)
{
	pw_log_level = level;
	global_log->level = level;

	update_all_topic_levels();
}

static int add_pattern(struct spa_list *list, const char *str, enum spa_log_level level)
{
	struct pattern *pattern;
	size_t len = strlen(str);

	pattern = calloc(1, sizeof(struct pattern) + len + 1);
	if (!pattern)
		return -errno;

	pattern->level = level;
	memcpy(pattern->pattern, str, len);
	spa_list_append(list, &pattern->link);
	return 0;
}

static bool
parse_log_level(const char *str, enum spa_log_level *l)
{
	uint32_t lvl;

	if (!str)
		return false;

	if (strlen(str) == 1) {
		/* SPA levels, plus a few duplicate codes that
		 * WirePlumber supports for some GLib levels. */
		switch (str[0]) {
		case 'X': lvl = SPA_LOG_LEVEL_NONE; break;
		case 'F': lvl = SPA_LOG_LEVEL_NONE; break; /* fatal */
		case 'E': lvl = SPA_LOG_LEVEL_ERROR; break;
		case 'W': lvl = SPA_LOG_LEVEL_WARN; break;
		case 'N': lvl = SPA_LOG_LEVEL_WARN; break; /* notice */
		case 'I': lvl = SPA_LOG_LEVEL_INFO; break;
		case 'D': lvl = SPA_LOG_LEVEL_DEBUG; break;
		case 'T': lvl = SPA_LOG_LEVEL_TRACE; break;
		default:
			goto check_int;
		}
	} else {
	check_int:
		if (!spa_atou32(str, &lvl, 0))
			return false;
		if (lvl > SPA_LOG_LEVEL_TRACE)
			return false;
	}

	*l = lvl;
	return true;
}

static int
parse_log_string(const char *str, struct spa_list *list, enum spa_log_level *level)
{
	struct spa_list new_patterns = SPA_LIST_INIT(&new_patterns);
	int n_tokens;

	*level = DEFAULT_LOG_LEVEL;
	if (!str || !*str)
		return 0;

	spa_auto(pw_strv) tokens = pw_split_strv(str, ",", INT_MAX, &n_tokens);
	if (n_tokens > 0) {
		int i;

		for (i = 0; i < n_tokens; i++) {
			int n_tok;
			char *tok[2];
			enum spa_log_level lvl;

			n_tok = pw_split_ip(tokens[i], ":", SPA_N_ELEMENTS(tok), tok);
			if (n_tok == 2 && parse_log_level(tok[1], &lvl)) {
				add_pattern(&new_patterns, tok[0], lvl);
			} else if (n_tok == 1 && parse_log_level(tok[0], &lvl)) {
				*level = lvl;
			} else {
				pw_log_warn("Ignoring invalid format in log level: '%s'",
						tokens[i]);
			}
		}
	}
	spa_list_insert_list(list, &new_patterns);
	return 0;
}

SPA_EXPORT
int pw_log_set_level_string(const char *str)
{
	struct spa_list new_patterns = SPA_LIST_INIT(&new_patterns);
	struct pattern *pattern;
	enum spa_log_level level;
	int res;

	if ((res = parse_log_string(str, &new_patterns, &level)) < 0)
		return res;

	pthread_mutex_lock(&topics_lock);

	spa_list_consume(pattern, &patterns, link) {
		spa_list_remove(&pattern->link);
		free(pattern);
	}

	spa_list_insert_list(&patterns, &new_patterns);

	pthread_mutex_unlock(&topics_lock);

	pw_log_set_level(level);
	return 0;
}

/** Log a message for the given topic
 * \param level the log level
 * \param topic the topic
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param ... printf style arguments to log
 *
 */
SPA_EXPORT
void
pw_log_logt(enum spa_log_level level,
	    const struct spa_log_topic *topic,
	    const char *file,
	    int line,
	    const char *func,
	    const char *fmt, ...)
{
	if (SPA_UNLIKELY(pw_log_topic_enabled(level, topic))) {
		va_list args;
		va_start(args, fmt);
		spa_log_logtv(global_log, level, topic, file, line, func, fmt, args);
		va_end(args);
	}
}

/** Log a message for the given topic with va_list
 * \param level the log level
 * \param topic the topic
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param args a va_list of arguments
 *
 */
SPA_EXPORT
void
pw_log_logtv(enum spa_log_level level,
	     const struct spa_log_topic *topic,
	     const char *file,
	     int line,
	     const char *func,
	     const char *fmt,
	     va_list args)
{
	spa_log_logtv(global_log, level, topic, file, line, func, fmt, args);
}


/** Log a message for the default topic with va_list
 * \param level the log level
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param args a va_list of arguments
 *
 */
SPA_EXPORT
void
pw_log_logv(enum spa_log_level level,
	    const char *file,
	    int line,
	    const char *func,
	    const char *fmt,
	    va_list args)
{
	pw_log_logtv(level, PW_LOG_TOPIC_DEFAULT, file, line, func, fmt, args);
}

/** Log a message for the default topic
 * \param level the log level
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param ... printf style arguments to log
 *
 */
SPA_EXPORT
void
pw_log_log(enum spa_log_level level,
	   const char *file,
	   int line,
	   const char *func,
	   const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	pw_log_logtv(level, PW_LOG_TOPIC_DEFAULT, file, line, func, fmt, args);
	va_end(args);
}

/** \fn void pw_log_error (const char *format, ...)
 * Log an error message
 * \param format a printf style format
 * \param ... printf style arguments
 */
/** \fn void pw_log_warn (const char *format, ...)
 * Log a warning message
 * \param format a printf style format
 * \param ... printf style arguments
 */
/** \fn void pw_log_info (const char *format, ...)
 * Log an info message
 * \param format a printf style format
 * \param ... printf style arguments
 */
/** \fn void pw_log_debug (const char *format, ...)
 * Log a debug message
 * \param format a printf style format
 * \param ... printf style arguments
 */
/** \fn void pw_log_trace (const char *format, ...)
 * Log a trace message. Trace messages may be generated from
 * \param format a printf style format
 * \param ... printf style arguments
 * realtime threads
 */

#include <spa/debug/pod.h>
#include <spa/debug/log.h>

void pw_log_log_object(enum spa_log_level level,
	const struct spa_log_topic *topic, const char *file,
	int line, const char *func, uint32_t flags, const void *object)
{
	struct spa_debug_log_ctx ctx = SPA_LOGF_DEBUG_INIT(global_log, level,
			topic, file, line, func );
	if (object == NULL) {
		pw_log_logt(level, topic, file, line, func, "NULL");
	} else {
		const struct spa_pod *pod = object;
		if (flags & PW_LOG_OBJECT_POD)
			spa_debugc_pod(&ctx.ctx, 0, SPA_TYPE_ROOT, pod);
		else if (flags & PW_LOG_OBJECT_FORMAT)
			spa_debugc_format(&ctx.ctx, 0, NULL, pod);
	}
}

void
pw_log_init(void)
{
}

void
pw_log_deinit(void)
{
	struct pattern *pattern;

	pthread_mutex_lock(&topics_lock);

	spa_list_consume(pattern, &patterns, link) {
		spa_list_remove(&pattern->link);
		free(pattern);
	}

	pthread_mutex_unlock(&topics_lock);

	/* don't free log topics, since they usually won't get re-registered */

	pw_log_set(NULL);
}
