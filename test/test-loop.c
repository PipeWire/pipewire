/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

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

static inline void write_eventfd(int evfd)
{
	uint64_t value = 1;
	ssize_t r = write(evfd, &value, sizeof(value));
	pwtest_errno_ok(r);
	pwtest_int_eq(r, (ssize_t) sizeof(value));
}

static inline void read_eventfd(int evfd)
{
	uint64_t value = 0;
	ssize_t r = read(evfd, &value, sizeof(value));
	pwtest_errno_ok(r);
	pwtest_int_eq(r, (ssize_t) sizeof(value));
}

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

	write_eventfd(data.a->source.fd);
	write_eventfd(data.b->source.fd);

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

	++d->count;
	pwtest_int_lt(d->count, 3);

	read_eventfd(source->fd);

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

	write_eventfd(data.a->source.fd);
	write_eventfd(data.b->source.fd);

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

	++d->count;
	pwtest_int_lt(d->count, 3);

	read_eventfd(source->fd);

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

	write_eventfd(data.a->source.fd);
	write_eventfd(data.b->source.fd);

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	return PWTEST_PASS;
}

static void
on_event_fail_if_called(void *data, int fd, uint32_t mask)
{
	pwtest_fail_if_reached();
}

struct dmsbd_data {
	struct pw_loop *l;
	struct pw_main_loop *ml;
	struct spa_source *source;
	struct spa_hook hook;
};

static void dmsbd_before(void *data)
{
}

static void dmsbd_after(void *data)
{
	struct dmsbd_data *d = data;

	pw_loop_destroy_source(d->l, d->source);
	pw_main_loop_quit(d->ml);
}

static const struct spa_loop_control_hooks dmsbd_hooks = {
	SPA_VERSION_LOOP_CONTROL_HOOKS,
	.before = dmsbd_before,
	.after = dmsbd_after,
};

PWTEST(destroy_managed_source_before_dispatch)
{
	pw_init(NULL, NULL);

	struct dmsbd_data data = {0};

	data.ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(data.ml);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.source = pw_loop_add_io(data.l, eventfd(0, 0), SPA_IO_IN, true, on_event_fail_if_called, NULL);
	pwtest_ptr_notnull(data.source);

	pw_loop_add_hook(data.l, &data.hook, &dmsbd_hooks, &data);

	write_eventfd(data.source->fd);

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	return PWTEST_PASS;
}

struct dmsbd_recurse_data {
	struct pw_loop *l;
	struct pw_main_loop *ml;
	struct spa_source *a, *b;
	struct spa_hook hook;
	bool first;
};

static void dmsbd_recurse_on_event(void *data, int fd, uint32_t mask)
{
	struct dmsbd_recurse_data *d = data;

	read_eventfd(fd);

	pw_loop_enter(d->l);
	pw_loop_iterate(d->l, 0);
	pw_loop_leave(d->l);

	pw_main_loop_quit(d->ml);
}

static void dmswp_recurse_before(void *data)
{
	struct dmsbd_recurse_data *d = data;

	if (d->first) {
		write_eventfd(d->a->fd);
		write_eventfd(d->b->fd);
	}
}

static void dmsbd_recurse_after(void *data)
{
	struct dmsbd_recurse_data *d = data;

	if (d->first) {
		pw_loop_destroy_source(d->l, d->b);

		d->first = false;
	}
}

static const struct spa_loop_control_hooks dmsbd_recurse_hooks = {
	SPA_VERSION_LOOP_CONTROL_HOOKS,
	.before = dmswp_recurse_before,
	.after = dmsbd_recurse_after,
};

PWTEST(destroy_managed_source_before_dispatch_recurse)
{
	pw_init(NULL, NULL);

	struct dmsbd_recurse_data data = {
		.first = true,
	};

	data.ml = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(data.ml);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.l = pw_main_loop_get_loop(data.ml);
	pwtest_ptr_notnull(data.l);

	data.a = pw_loop_add_io(data.l, eventfd(0, 0), SPA_IO_IN, true, dmsbd_recurse_on_event, &data);
	data.b = pw_loop_add_io(data.l, eventfd(0, 0), SPA_IO_IN, true, on_event_fail_if_called, NULL);
	pwtest_ptr_notnull(data.a);
	pwtest_ptr_notnull(data.b);

	pw_loop_add_hook(data.l, &data.hook, &dmsbd_recurse_hooks, &data);

	pw_main_loop_run(data.ml);
	pw_main_loop_destroy(data.ml);

	pw_deinit();

	return PWTEST_PASS;
}

struct ctwd_data {
	struct spa_source source;
	int handler_running_barrier;
};

static void ctwd_event_handler(struct spa_source *source)
{
	struct ctwd_data *data = source->data;

	write_eventfd(data->handler_running_barrier);

	for (;;)
		pause(); /* the purpose of this is to block the loop */
}

static int ctwd_add_source(struct spa_loop *loop, bool async, uint32_t seq,
			   const void *d, size_t size, void *user_data)
{
	struct ctwd_data *data = user_data;

	pwtest_neg_errno_ok(spa_loop_add_source(loop, &data->source));

	return 0;
}

PWTEST(cancel_thread_while_dispatching)
{
	static const struct spa_dict_item data_loop_props_items[] = {
		{ "loop.cancel", "true" },
	};
	static const struct spa_dict data_loop_props = SPA_DICT_INIT_ARRAY(data_loop_props_items);

	struct ctwd_data data = {
		.source = {
			.data = &data,
			.func = ctwd_event_handler,
			.mask = SPA_IO_IN,
			.fd = eventfd(0, 0),
		},
		.handler_running_barrier = eventfd(0, 0),
	};

	pw_init(NULL, NULL);

	struct pw_data_loop *dl = pw_data_loop_new(&data_loop_props);
	pwtest_ptr_notnull(dl);

	struct pw_loop *l = pw_data_loop_get_loop(dl);
	pwtest_ptr_notnull(l);

	pwtest_neg_errno_ok(pw_data_loop_start(dl));

	pw_loop_invoke(l, ctwd_add_source, 0, NULL, 0, true, &data);
	pwtest_ptr_notnull(data.source.loop);

	write_eventfd(data.source.fd);
	read_eventfd(data.handler_running_barrier);

	pwtest_neg_errno_ok(pw_data_loop_stop(dl));

	/* these are the important checks */
	pwtest_ptr_null(data.source.priv);
	pwtest_int_eq(data.source.rmask, UINT32_C(0));

	pw_loop_remove_source(l, &data.source);

	pw_data_loop_destroy(dl);

	close(data.source.fd);
	close(data.handler_running_barrier);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(support)
{
	pwtest_add(pwtest_loop_destroy2, PWTEST_NOARG);
	pwtest_add(pwtest_loop_recurse1, PWTEST_NOARG);
	pwtest_add(pwtest_loop_recurse2, PWTEST_NOARG);
	pwtest_add(destroy_managed_source_before_dispatch, PWTEST_NOARG);
	pwtest_add(destroy_managed_source_before_dispatch_recurse, PWTEST_NOARG);
	pwtest_add(cancel_thread_while_dispatching, PWTEST_NOARG);

	return PWTEST_PASS;
}
