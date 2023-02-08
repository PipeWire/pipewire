/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <limits.h>
#include <fnmatch.h>

#include <spa/support/log-impl.h>

#include <spa/pod/pod.h>
#include <spa/debug/types.h>
#include <spa/debug/format.h>
#include <spa/pod/iter.h>
#include <spa/utils/list.h>

#include <pipewire/log.h>
#include <pipewire/private.h>

SPA_LOG_IMPL(default_log);

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_WARN

SPA_EXPORT
enum spa_log_level pw_log_level = DEFAULT_LOG_LEVEL;

static struct spa_log *global_log = &default_log.log;

SPA_EXPORT
struct spa_log_topic *PW_LOG_TOPIC_DEFAULT;

PW_LOG_TOPIC_STATIC(log_topic, "pw.log"); /* log topic for this file here */
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
PW_LOG_TOPIC(log_work_queue, "pw.work-queue");

PW_LOG_TOPIC(PW_LOG_TOPIC_DEFAULT, "default");

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

SPA_EXPORT
void
_pw_log_topic_new(struct spa_log_topic *topic)
{
	spa_log_topic_init(global_log, topic);
}

void
pw_log_init(void)
{
	PW_LOG_TOPIC_INIT(PW_LOG_TOPIC_DEFAULT);
	PW_LOG_TOPIC_INIT(log_buffers);
	PW_LOG_TOPIC_INIT(log_client);
	PW_LOG_TOPIC_INIT(log_conf);
	PW_LOG_TOPIC_INIT(log_context);
	PW_LOG_TOPIC_INIT(log_core);
	PW_LOG_TOPIC_INIT(log_data_loop);
	PW_LOG_TOPIC_INIT(log_device);
	PW_LOG_TOPIC_INIT(log_factory);
	PW_LOG_TOPIC_INIT(log_filter);
	PW_LOG_TOPIC_INIT(log_global);
	PW_LOG_TOPIC_INIT(log_link);
	PW_LOG_TOPIC_INIT(log_loop);
	PW_LOG_TOPIC_INIT(log_main_loop);
	PW_LOG_TOPIC_INIT(log_mem);
	PW_LOG_TOPIC_INIT(log_metadata);
	PW_LOG_TOPIC_INIT(log_module);
	PW_LOG_TOPIC_INIT(log_node);
	PW_LOG_TOPIC_INIT(log_port);
	PW_LOG_TOPIC_INIT(log_properties);
	PW_LOG_TOPIC_INIT(log_protocol);
	PW_LOG_TOPIC_INIT(log_proxy);
	PW_LOG_TOPIC_INIT(log_resource);
	PW_LOG_TOPIC_INIT(log_stream);
	PW_LOG_TOPIC_INIT(log_thread_loop);
	PW_LOG_TOPIC_INIT(log_topic);
	PW_LOG_TOPIC_INIT(log_work_queue);
}
