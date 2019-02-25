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

#ifndef SPA_UTILS_RESULT_H
#define SPA_UTILS_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/list.h>

#define SPA_ASYNC_BIT			(1 << 30)
#define SPA_ASYNC_MASK			(3 << 30)
#define SPA_ASYNC_SEQ_MASK		(SPA_ASYNC_BIT - 1)

#define SPA_RESULT_IS_OK(res)		((res) >= 0)
#define SPA_RESULT_IS_ERROR(res)	((res) < 0)
#define SPA_RESULT_IS_ASYNC(res)	(((res) & SPA_ASYNC_MASK) == SPA_ASYNC_BIT)

#define SPA_RESULT_ASYNC_SEQ(res)	((res) & SPA_ASYNC_SEQ_MASK)
#define SPA_RESULT_RETURN_ASYNC(seq)	(SPA_ASYNC_BIT | SPA_RESULT_ASYNC_SEQ(seq))

struct spa_pending;

typedef int (*spa_pending_func_t) (struct spa_pending *pending, const void *result);

struct spa_pending {
	struct spa_list link;		/**< link used internally */
	int seq;			/**< sequence number of pending result */
	int res;			/**< result code of operation, valid in callback */
	spa_pending_func_t func;	/**< callback function */
	void *data;			/**< extra user data */
};

static inline void spa_pending_remove(struct spa_pending *pending)
{
	spa_list_remove(&pending->link);
}

struct spa_pending_queue {
	struct spa_list pending;
	int seq;
};

static inline void spa_pending_queue_init(struct spa_pending_queue *queue)
{
	spa_list_init(&queue->pending);
}

static inline void spa_pending_queue_add(struct spa_pending_queue *queue,
		int seq, struct spa_pending *pending, spa_pending_func_t func, void *data)
{
	pending->seq = seq;
	pending->func = func;
	pending->data = data;
	spa_list_append(&queue->pending, &pending->link);
}

static inline int spa_pending_queue_complete(struct spa_pending_queue *queue,
		int seq, int res, const void *result)
{
	struct spa_pending *p, *t;
	spa_list_for_each_safe(p, t, &queue->pending, link) {
		if (p->seq == seq) {
			p->res = res;
			spa_pending_remove(p);
			p->func(p, result);
		}
	}
	return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_RESULT_H */
