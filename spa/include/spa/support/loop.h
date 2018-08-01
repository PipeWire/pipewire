/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_LOOP_H__
#define __SPA_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_loop;
#define SPA_TYPE__Loop		SPA_TYPE_INTERFACE_BASE "Loop"
#define SPA_TYPE_LOOP_BASE	SPA_TYPE__Loop ":"

struct spa_loop_control;
#define SPA_TYPE__LoopControl	SPA_TYPE_INTERFACE_BASE "LoopControl"
struct spa_loop_utils;
#define SPA_TYPE__LoopUtils	SPA_TYPE_INTERFACE_BASE "LoopUtils"

#define SPA_TYPE_LOOP__MainLoop		SPA_TYPE_LOOP_BASE "MainLoop"
#define SPA_TYPE_LOOP__DataLoop		SPA_TYPE_LOOP_BASE "DataLoop"

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

enum spa_io {
	SPA_IO_IN = (1 << 0),
	SPA_IO_OUT = (1 << 1),
	SPA_IO_HUP = (1 << 2),
	SPA_IO_ERR = (1 << 3),
};

struct spa_source;

typedef void (*spa_source_func_t) (struct spa_source *source);

struct spa_source {
	struct spa_loop *loop;
	spa_source_func_t func;
	void *data;
	int fd;
	enum spa_io mask;
	enum spa_io rmask;
};

typedef int (*spa_invoke_func_t) (struct spa_loop *loop,
				  bool async,
				  uint32_t seq,
				  const void *data,
				  size_t size,
				  void *user_data);

/**
 * Register sources and work items to an event loop
 */
struct spa_loop {
	/* the version of this structure. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_LOOP	0
	uint32_t version;

	/** add a source to the loop */
	int (*add_source) (struct spa_loop *loop,
			   struct spa_source *source);

	/** update the source io mask */
	int (*update_source) (struct spa_source *source);

	/** remove a source from the loop */
	void (*remove_source) (struct spa_source *source);

	/** invoke a function in the context of this loop */
	int (*invoke) (struct spa_loop *loop,
		       spa_invoke_func_t func,
		       uint32_t seq,
		       const void *data,
		       size_t size,
		       bool block,
		       void *user_data);
};

#define spa_loop_add_source(l,...)	(l)->add_source((l),__VA_ARGS__)
#define spa_loop_update_source(l,...)	(l)->update_source(__VA_ARGS__)
#define spa_loop_remove_source(l,...)	(l)->remove_source(__VA_ARGS__)
#define spa_loop_invoke(l,...)		(l)->invoke((l),__VA_ARGS__)


/** Control hooks */
struct spa_loop_control_hooks {
#define SPA_VERSION_LOOP_CONTROL_HOOKS	0
	uint32_t version;
	/** Executed right before waiting for events */
	void (*before) (void *data);
	/** Executed right after waiting for events */
	void (*after) (void *data);
};

#define spa_loop_control_hook_before(l) spa_hook_list_call(l, struct spa_loop_control_hooks, before, 0)
#define spa_loop_control_hook_after(l) spa_hook_list_call(l, struct spa_loop_control_hooks, after, 0)

/**
 * Control an event loop
 */
struct spa_loop_control {
	/* the version of this structure. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_LOOP_CONTROL	0
	uint32_t version;

	int (*get_fd) (struct spa_loop_control *ctrl);

	/** Add a hook
	 * \param ctrl the control to change
	 * \param hooks the hooks to add */
	void (*add_hook) (struct spa_loop_control *ctrl,
			  struct spa_hook *hook,
			  const struct spa_loop_control_hooks *hooks,
			  void *data);

	void (*enter) (struct spa_loop_control *ctrl);
	void (*leave) (struct spa_loop_control *ctrl);

	int (*iterate) (struct spa_loop_control *ctrl, int timeout);
};

#define spa_loop_control_get_fd(l)		(l)->get_fd(l)
#define spa_loop_control_add_hook(l,...)	(l)->add_hook((l),__VA_ARGS__)
#define spa_loop_control_enter(l)		(l)->enter(l)
#define spa_loop_control_iterate(l,...)		(l)->iterate((l),__VA_ARGS__)
#define spa_loop_control_leave(l)		(l)->leave(l)


typedef void (*spa_source_io_func_t) (void *data, int fd, enum spa_io mask);
typedef void (*spa_source_idle_func_t) (void *data);
typedef void (*spa_source_event_func_t) (void *data, uint64_t count);
typedef void (*spa_source_timer_func_t) (void *data, uint64_t expirations);
typedef void (*spa_source_signal_func_t) (void *data, int signal_number);

/**
 * Create sources for an event loop
 */
struct spa_loop_utils {
	/* the version of this structure. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_LOOP_UTILS	0
	uint32_t version;

	struct spa_source *(*add_io) (struct spa_loop_utils *utils,
				      int fd,
				      enum spa_io mask,
				      bool close,
				      spa_source_io_func_t func, void *data);

	int (*update_io) (struct spa_source *source, enum spa_io mask);

	struct spa_source *(*add_idle) (struct spa_loop_utils *utils,
					bool enabled,
					spa_source_idle_func_t func, void *data);
	void (*enable_idle) (struct spa_source *source, bool enabled);

	struct spa_source *(*add_event) (struct spa_loop_utils *utils,
					 spa_source_event_func_t func, void *data);
	void (*signal_event) (struct spa_source *source);

	struct spa_source *(*add_timer) (struct spa_loop_utils *utils,
					 spa_source_timer_func_t func, void *data);
	int (*update_timer) (struct spa_source *source,
			     struct timespec *value,
			     struct timespec *interval,
			     bool absolute);
	struct spa_source *(*add_signal) (struct spa_loop_utils *utils,
					  int signal_number,
					  spa_source_signal_func_t func, void *data);

	/** destroy a source allocated with this interface. This function
	 * should only be called when the loop is not running or from the
	 * context of the running loop */
	void (*destroy_source) (struct spa_source *source);
};

#define spa_loop_utils_add_io(l,...)		(l)->add_io(l,__VA_ARGS__)
#define spa_loop_utils_update_io(l,...)		(l)->update_io(__VA_ARGS__)
#define spa_loop_utils_add_idle(l,...)		(l)->add_idle(l,__VA_ARGS__)
#define spa_loop_utils_enable_idle(l,...)	(l)->enable_idle(__VA_ARGS__)
#define spa_loop_utils_add_event(l,...)		(l)->add_event(l,__VA_ARGS__)
#define spa_loop_utils_signal_event(l,...)	(l)->signal_event(__VA_ARGS__)
#define spa_loop_utils_add_timer(l,...)		(l)->add_timer(l,__VA_ARGS__)
#define spa_loop_utils_update_timer(l,...)	(l)->update_timer(__VA_ARGS__)
#define spa_loop_utils_add_signal(l,...)	(l)->add_signal(l,__VA_ARGS__)
#define spa_loop_utils_destroy_source(l,...)	(l)->destroy_source(__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_LOOP_H__ */
