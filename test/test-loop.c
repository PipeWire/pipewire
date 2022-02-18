/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "pwtest.h"

#include <pipewire/pipewire.h>

struct obj {
	int x;
	struct spa_source source;
};

struct data {
	struct pw_main_loop *ml;
	struct pw_loop *l;
	struct obj *a, *b;
	int count;
};

static void on_event(struct spa_source *source)
{
	struct data *d = source->data;

	pw_loop_remove_source(d->l, &d->a->source);
	pw_loop_remove_source(d->l, &d->b->source);
	close(d->a->source.fd);
	close(d->b->source.fd);
	free(d->a);
	free(d->b);

	pw_main_loop_quit(d->ml);
}

PWTEST(pwtest_loop_destroy2)
{
	struct data data;

	pw_init(0, NULL);

	spa_zero(data);
	data.ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(data.ml);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.a = calloc(1, sizeof(*data.a));
	data.b = calloc(1, sizeof(*data.b));

	data.a->source.func = on_event;
	data.a->source.fd = eventfd(0, 0);
	data.a->source.mask = SPA_IO_IN;
	data.a->source.data = &data;
	data.b->source.func = on_event;
	data.b->source.fd = eventfd(0, 0);
	data.b->source.mask = SPA_IO_IN;
	data.b->source.data = &data;

	pw_loop_add_source(data.l, &data.a->source);
	pw_loop_add_source(data.l, &data.b->source);

	write(data.a->source.fd, &(uint64_t){1}, sizeof(uint64_t));
	write(data.b->source.fd, &(uint64_t){1}, sizeof(uint64_t));

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	return PWTEST_PASS;
}

static void
on_event_recurse1(struct spa_source *source)
{
	static bool first = true;
	struct data *d = source->data;
	uint64_t val;

	++d->count;
	pwtest_int_lt(d->count, 3);

	read(source->fd, &val, sizeof(val));

	if (first) {
		first = false;
		pw_loop_enter(d->l);
		pw_loop_iterate(d->l, -1);
		pw_loop_leave(d->l);
	}
	pw_main_loop_quit(d->ml);
}

PWTEST(pwtest_loop_recurse1)
{
	struct data data;

	pw_init(0, NULL);

	spa_zero(data);
	data.ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(data.ml);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.a = calloc(1, sizeof(*data.a));
	data.b = calloc(1, sizeof(*data.b));

	data.a->source.func = on_event_recurse1;
	data.a->source.fd = eventfd(0, 0);
	data.a->source.mask = SPA_IO_IN;
	data.a->source.data = &data;
	data.b->source.func = on_event_recurse1;
	data.b->source.fd = eventfd(0, 0);
	data.b->source.mask = SPA_IO_IN;
	data.b->source.data = &data;

	pw_loop_add_source(data.l, &data.a->source);
	pw_loop_add_source(data.l, &data.b->source);

	write(data.a->source.fd, &(uint64_t){1}, sizeof(uint64_t));
	write(data.b->source.fd, &(uint64_t){1}, sizeof(uint64_t));

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	free(data.a);
	free(data.b);

	return PWTEST_PASS;
}

static void
on_event_recurse2(struct spa_source *source)
{
	static bool first = true;
	struct data *d = source->data;
	uint64_t val;

	++d->count;
	pwtest_int_lt(d->count, 3);

	read(source->fd, &val, sizeof(val));

	if (first) {
		first = false;
		pw_loop_enter(d->l);
		pw_loop_iterate(d->l, -1);
		pw_loop_leave(d->l);
	} else {
		pw_loop_remove_source(d->l, &d->a->source);
		pw_loop_remove_source(d->l, &d->b->source);
		close(d->a->source.fd);
		close(d->b->source.fd);
		free(d->a);
		free(d->b);
	}
	pw_main_loop_quit(d->ml);
}

PWTEST(pwtest_loop_recurse2)
{
	struct data data;

	pw_init(0, NULL);

	spa_zero(data);
	data.ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(data.ml);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.a = calloc(1, sizeof(*data.a));
	data.b = calloc(1, sizeof(*data.b));

	data.a->source.func = on_event_recurse2;
	data.a->source.fd = eventfd(0, 0);
	data.a->source.mask = SPA_IO_IN;
	data.a->source.data = &data;
	data.b->source.func = on_event_recurse2;
	data.b->source.fd = eventfd(0, 0);
	data.b->source.mask = SPA_IO_IN;
	data.b->source.data = &data;

	pw_loop_add_source(data.l, &data.a->source);
	pw_loop_add_source(data.l, &data.b->source);

	write(data.a->source.fd, &(uint64_t){1}, sizeof(uint64_t));
	write(data.b->source.fd, &(uint64_t){1}, sizeof(uint64_t));

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(thread_loop_destroy_between_poll_and_lock)
{
	pw_init(NULL, NULL);

	struct pw_thread_loop *thread_loop = pw_thread_loop_new("uaf", NULL);
	pwtest_ptr_notnull(thread_loop);

	struct pw_loop *loop = pw_thread_loop_get_loop(thread_loop);
	pwtest_ptr_notnull(loop);

	int evfd = eventfd(0, 0);
	pwtest_errno_ok(evfd);

	struct spa_source *source = pw_loop_add_io(loop, evfd, SPA_IO_IN, true, NULL, NULL);
	pwtest_ptr_notnull(source);

	pw_thread_loop_start(thread_loop);

	pw_thread_loop_lock(thread_loop);
	{
		write(evfd, &(uint64_t){1}, sizeof(uint64_t));
		sleep(1);
		pw_loop_destroy_source(loop, source);
	}
	pw_thread_loop_unlock(thread_loop);

	pw_thread_loop_destroy(thread_loop);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(support)
{
	pwtest_add(pwtest_loop_destroy2, PWTEST_NOARG);
	pwtest_add(pwtest_loop_recurse1, PWTEST_NOARG);
	pwtest_add(pwtest_loop_recurse2, PWTEST_NOARG);
	pwtest_add(thread_loop_destroy_between_poll_and_lock, PWTEST_NOARG);

	return PWTEST_PASS;
}
