/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __SPA_LOOP_H__
#define __SPA_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_loop;
struct spa_loop_control;
struct spa_loop_utils;
struct spa_source;

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

enum spa_io {
	SPA_IO_IN = (1 << 0),
	SPA_IO_OUT = (1 << 1),
	SPA_IO_HUP = (1 << 2),
	SPA_IO_ERR = (1 << 3),
};

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
	/** Executed right before waiting for events. It is typically used to
	 * release locks. */
	void (*before) (void *data);
	/** Executed right after waiting for events. It is typically used to
	 * reacquire locks. */
	void (*after) (void *data);
};

#define spa_loop_control_hook_before(l) spa_hook_list_call_simple(l, struct spa_loop_control_hooks, before, 0)
#define spa_loop_control_hook_after(l) spa_hook_list_call_simple(l, struct spa_loop_control_hooks, after, 0)

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
	 * \param hooks the hooks to add
	 *
	 * Adds hooks to the loop controlled by \a ctrl.
	 */
	void (*add_hook) (struct spa_loop_control *ctrl,
			  struct spa_hook *hook,
			  const struct spa_loop_control_hooks *hooks,
			  void *data);

	/** Enter a loop
	 * \param ctrl the control
	 *
	 * Start an interation of the loop. This function should be called
	 * before calling iterate and is typically used to capture the thread
	 * that this loop will run in.
	 */
	void (*enter) (struct spa_loop_control *ctrl);
	/** Leave a loop
	 * \param ctrl the control
	 *
	 * Ends the iteration of a loop. This should be called after calling
	 * iterate.
	 */
	void (*leave) (struct spa_loop_control *ctrl);

	/** Perform one iteration of the loop.
	 * \param ctrl the control
	 * \param timeout an optional timeout. 0 for no timeout, -1 for infinte
	 *		timeout.
	 *
	 * This function will block
	 * up to \a timeout and then dispatch the fds with activity.
	 * The number of dispatched fds is returned.
	 */
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
