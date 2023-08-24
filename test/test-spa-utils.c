/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include <locale.h>
#include <unistd.h>
#include <sys/wait.h>

#include <valgrind/valgrind.h>

#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/dict.h>
#include <spa/utils/list.h>
#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/utils/ansi.h>

#include "pwtest.h"

PWTEST(utils_abi_sizes)
{
#if defined(__x86_64__) && defined(__LP64__)
	/* dict */
	pwtest_int_eq(sizeof(struct spa_dict_item), 16U);
	pwtest_int_eq(sizeof(struct spa_dict), 16U);

	/* hook */
	pwtest_int_eq(sizeof(struct spa_hook_list), sizeof(struct spa_list));
	pwtest_int_eq(sizeof(struct spa_hook), 48U);

	/* list */
	pwtest_int_eq(sizeof(struct spa_list), 16U);

	return PWTEST_PASS;
#endif

	return PWTEST_SKIP;
}

PWTEST(utils_abi)
{
	/* defs */
	pwtest_int_eq(SPA_DIRECTION_INPUT, 0);
	pwtest_int_eq(SPA_DIRECTION_OUTPUT, 1);

	pwtest_int_eq(sizeof(struct spa_rectangle), 8U);
	pwtest_int_eq(sizeof(struct spa_point), 8U);
	pwtest_int_eq(sizeof(struct spa_region), 16U);
	pwtest_int_eq(sizeof(struct spa_fraction), 8U);

	{
		struct spa_rectangle r = SPA_RECTANGLE(12, 14);
		pwtest_int_eq(r.width, 12U);
		pwtest_int_eq(r.height, 14U);
	}
	{
		struct spa_point p = SPA_POINT(8, 34);
		pwtest_int_eq(p.x, 8);
		pwtest_int_eq(p.y, 34);
	}
	{
		struct spa_region r = SPA_REGION(4, 5, 12, 13);
		pwtest_int_eq(r.position.x, 4);
		pwtest_int_eq(r.position.y, 5);
		pwtest_int_eq(r.size.width, 12U);
		pwtest_int_eq(r.size.height, 13U);
	}
	{
		struct spa_fraction f = SPA_FRACTION(56, 125);
		pwtest_int_eq(f.num, 56U);
		pwtest_int_eq(f.denom, 125U);
	}

	/* ringbuffer */
	pwtest_int_eq(sizeof(struct spa_ringbuffer), 8U);

	/* type */
	pwtest_int_eq(SPA_TYPE_START, 0);
	pwtest_int_eq(SPA_TYPE_None, 1);
	pwtest_int_eq(SPA_TYPE_Bool, 2);
	pwtest_int_eq(SPA_TYPE_Id, 3);
	pwtest_int_eq(SPA_TYPE_Int, 4);
	pwtest_int_eq(SPA_TYPE_Long, 5);
	pwtest_int_eq(SPA_TYPE_Float, 6);
	pwtest_int_eq(SPA_TYPE_Double, 7);
	pwtest_int_eq(SPA_TYPE_String, 8);
	pwtest_int_eq(SPA_TYPE_Bytes, 9);
	pwtest_int_eq(SPA_TYPE_Rectangle, 10);
	pwtest_int_eq(SPA_TYPE_Fraction, 11);
	pwtest_int_eq(SPA_TYPE_Bitmap, 12);
	pwtest_int_eq(SPA_TYPE_Array, 13);
	pwtest_int_eq(SPA_TYPE_Struct, 14);
	pwtest_int_eq(SPA_TYPE_Object, 15);
	pwtest_int_eq(SPA_TYPE_Sequence, 16);
	pwtest_int_eq(SPA_TYPE_Pointer, 17);
	pwtest_int_eq(SPA_TYPE_Fd, 18);
	pwtest_int_eq(SPA_TYPE_Choice, 19);
	pwtest_int_eq(SPA_TYPE_Pod, 20);
	pwtest_int_eq(_SPA_TYPE_LAST, 21);

	pwtest_int_eq(SPA_TYPE_EVENT_START, 0x20000);
	pwtest_int_eq(SPA_TYPE_EVENT_Device, 0x20001);
	pwtest_int_eq(SPA_TYPE_EVENT_Node, 0x20002);
	pwtest_int_eq(_SPA_TYPE_EVENT_LAST, 0x20003);

	pwtest_int_eq(SPA_TYPE_COMMAND_START, 0x30000);
	pwtest_int_eq(SPA_TYPE_COMMAND_Device, 0x30001);
	pwtest_int_eq(SPA_TYPE_COMMAND_Node, 0x30002);
	pwtest_int_eq(_SPA_TYPE_COMMAND_LAST, 0x30003);

	pwtest_int_eq(SPA_TYPE_OBJECT_START, 0x40000);
	pwtest_int_eq(SPA_TYPE_OBJECT_PropInfo, 0x40001);
	pwtest_int_eq(SPA_TYPE_OBJECT_Props, 0x40002);
	pwtest_int_eq(SPA_TYPE_OBJECT_Format, 0x40003);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamBuffers, 0x40004);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamMeta, 0x40005);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamIO, 0x40006);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamProfile, 0x40007);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamPortConfig, 0x40008);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamRoute, 0x40009);
	pwtest_int_eq(SPA_TYPE_OBJECT_Profiler, 0x4000a);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamLatency, 0x4000b);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamProcessLatency, 0x4000c);
	pwtest_int_eq(SPA_TYPE_OBJECT_ParamTag, 0x4000d);
	pwtest_int_eq(_SPA_TYPE_OBJECT_LAST, 0x4000e);

	pwtest_int_eq(SPA_TYPE_VENDOR_PipeWire, 0x02000000);
	pwtest_int_eq(SPA_TYPE_VENDOR_Other, 0x7f000000);

	return PWTEST_PASS;
}

PWTEST(utils_macros)
{
	uint8_t ptr[64];
	uint16_t i16[14];
	uint32_t i32[10];
	uint64_t i64[12];
	unsigned char c[16];

	pwtest_int_eq(SPA_MIN(1, 2), 1);
	pwtest_int_eq(SPA_MIN(1, -2), -2);
	pwtest_int_eq(SPA_MAX(1, 2), 2);
	pwtest_int_eq(SPA_MAX(1, -2), 1);
	pwtest_int_eq(SPA_CLAMP(23, 1, 16), 16);
	pwtest_int_eq(SPA_CLAMP(-1, 1, 16), 1);
	pwtest_int_eq(SPA_CLAMP(8, 1, 16), 8);

	/* SPA_MEMBER exists for backwards compatibility but should no
	 * longer be used, let's make sure it does what we expect it to */
	pwtest_ptr_eq(SPA_MEMBER(ptr, 4, void), SPA_PTROFF(ptr, 4, void));
	pwtest_ptr_eq(SPA_MEMBER(ptr, 32, void), SPA_PTROFF(ptr, 32, void));
	pwtest_ptr_eq(SPA_MEMBER(ptr, 0, void), SPA_PTROFF(ptr, 0, void));
	pwtest_ptr_eq(SPA_MEMBER_ALIGN(ptr, 0, 4, void), SPA_PTROFF_ALIGN(ptr, 0, 4, void));
	pwtest_ptr_eq(SPA_MEMBER_ALIGN(ptr, 4, 32, void), SPA_PTROFF_ALIGN(ptr, 4, 32, void));

	pwtest_int_eq(SPA_N_ELEMENTS(ptr), 64U);
	pwtest_int_eq(SPA_N_ELEMENTS(i32), 10U);
	pwtest_int_eq(SPA_N_ELEMENTS(i64), 12U);
	pwtest_int_eq(SPA_N_ELEMENTS(i16), 14U);
	pwtest_int_eq(SPA_N_ELEMENTS(c), 16U);

#define check_traversal(array_) \
	{ \
		int count = 0; \
		SPA_FOR_EACH_ELEMENT_VAR(array_, it) \
			*it = count++; \
		for (size_t i = 0; i < SPA_N_ELEMENTS(array_); i++) \
			pwtest_int_eq(array_[i], i); \
	}
	check_traversal(ptr);
	check_traversal(i64);
	check_traversal(i32);
	check_traversal(i16);
	check_traversal(c);
	return PWTEST_PASS;
}

PWTEST(utils_result)
{
	int res;
	pwtest_int_eq(SPA_RESULT_IS_OK(0), true);
	pwtest_int_eq(SPA_RESULT_IS_OK(1), true);
	pwtest_int_eq(SPA_RESULT_IS_ERROR(0), false);
	pwtest_int_eq(SPA_RESULT_IS_ERROR(1), false);
	pwtest_int_eq(SPA_RESULT_IS_ERROR(-1), true);
	pwtest_int_eq(SPA_RESULT_IS_ASYNC(-1), false);
	pwtest_int_eq(SPA_RESULT_IS_ASYNC(0), false);
	res = SPA_RESULT_RETURN_ASYNC(11);
	pwtest_int_eq(SPA_RESULT_IS_ASYNC(res), true);
	pwtest_int_eq(SPA_RESULT_IS_ERROR(res), false);
	pwtest_int_eq(SPA_RESULT_IS_OK(res), true);
	pwtest_int_eq(SPA_RESULT_ASYNC_SEQ(res), 11);
	return PWTEST_PASS;
}

PWTEST(utils_dict)
{
	struct spa_dict_item items[5] = {
		SPA_DICT_ITEM_INIT("key", "value"),
		SPA_DICT_ITEM_INIT("pipe", "wire"),
		SPA_DICT_ITEM_INIT("test", "Works!"),
		SPA_DICT_ITEM_INIT("123", ""),
		SPA_DICT_ITEM_INIT("SPA", "Simple Plugin API"),
	};
	struct spa_dict dict = SPA_DICT_INIT_ARRAY (items);
	const struct spa_dict_item *it;
	int i = 0;

	pwtest_int_eq(dict.n_items, 5U);
	pwtest_str_eq(spa_dict_lookup(&dict, "pipe"), "wire");
	pwtest_str_eq(spa_dict_lookup(&dict, "123"), "");
	pwtest_str_eq(spa_dict_lookup(&dict, "key"), "value");
	pwtest_str_eq(spa_dict_lookup(&dict, "SPA"), "Simple Plugin API");
	pwtest_str_eq(spa_dict_lookup(&dict, "test"), "Works!");
	pwtest_ptr_null(spa_dict_lookup(&dict, "nonexistent"));

	pwtest_ptr_eq(spa_dict_lookup_item(&dict, "123"), &items[3]);
	pwtest_ptr_null(spa_dict_lookup_item(&dict, "foobar"));

	spa_dict_for_each(it, &dict) {
		pwtest_ptr_eq(it, &items[i++]);
	}
	pwtest_int_eq(i, 5);
	return PWTEST_PASS;
}

struct string_list {
	char string[20];
	struct spa_list node;
};

PWTEST(utils_list)
{
	struct string_list list;
	struct spa_list *head = &list.node;
	struct string_list *e;
	int i;

	spa_list_init(head);
	pwtest_bool_true(spa_list_is_empty(head));

	e = malloc(sizeof(struct string_list));
	strcpy(e->string, "test");
	spa_list_insert(head, &e->node);
	pwtest_bool_false(spa_list_is_empty(head));
	pwtest_ptr_eq(spa_list_first(head, struct string_list, node), e);
	pwtest_ptr_eq(spa_list_last(head, struct string_list, node), e);

	e = malloc(sizeof(struct string_list));
	strcpy(e->string, "pipewire!");
	spa_list_append(head, &e->node);
	pwtest_bool_false(spa_list_is_empty(head));
	pwtest_ptr_eq(spa_list_last(head, struct string_list, node), e);

	e = malloc(sizeof(struct string_list));
	strcpy(e->string, "First element");
	spa_list_prepend(head, &e->node);
	pwtest_bool_false(spa_list_is_empty(head));
	pwtest_ptr_eq(spa_list_first(head, struct string_list, node), e);

	i = 0;
	spa_list_for_each(e, head, node) {
		switch (i++) {
			case 0:
				pwtest_str_eq(e->string, "First element");
				break;
			case 1:
				pwtest_str_eq(e->string, "test");
				break;
			case 2:
				pwtest_str_eq(e->string, "pipewire!");
				break;
			default:
				pwtest_fail_if_reached();
				break;
		}
	}

	i = 0;
	spa_list_consume(e, head, node) {
		spa_list_remove(&e->node);
		free(e);
		i++;
	}
	pwtest_int_eq(i, 3);
	pwtest_bool_true(spa_list_is_empty(head));

	return PWTEST_PASS;
}


struct my_hook {
	int version;
	void (*invoke) (void *);
};

struct my_hook_data {
	bool cb1;
	bool cb2;
	bool cb3;
};

static void test_hook_callback_1(void *data)
{
	((struct my_hook_data *) data)->cb1 = true;
}

static void test_hook_callback_2(void *data)
{
	((struct my_hook_data *) data)->cb2 = true;
}

static void test_hook_callback_3(void *data)
{
	((struct my_hook_data *) data)->cb3 = true;
}

static void test_hook_callback_4(void *data)
{
	pwtest_fail_if_reached();
}

static int hook_free_count = 0;

static void hook_removed_cb(struct spa_hook *h)
{
	free(h);
	hook_free_count++;
}

PWTEST(utils_hook)
{
	const int HOOK_VERSION = 2;
	struct spa_hook_list hl;
	struct my_hook callbacks[4] = {
		{2, test_hook_callback_1},
		{3, test_hook_callback_2},
		{2, test_hook_callback_3},
		/* version 1 should not be called */
		{1, test_hook_callback_4}
	};
	struct my_hook_data data = {0};
	struct spa_hook *h;
	int count = 0;

	spa_hook_list_init(&hl);

	h = malloc(sizeof(struct spa_hook));
	spa_hook_list_append(&hl, h, &callbacks[1], &data);
	h->removed = hook_removed_cb;

	h = malloc(sizeof(struct spa_hook));
	spa_hook_list_append(&hl, h, &callbacks[2], &data);
	h->removed = hook_removed_cb;

	/* iterate with the simple API */
	spa_hook_list_call_simple(&hl, struct my_hook, invoke, HOOK_VERSION);
	pwtest_bool_eq(data.cb1, false);
	pwtest_bool_eq(data.cb2, true);
	pwtest_bool_eq(data.cb3, true);

	/* reset cb* booleans to false */
	memset(&data, 0, sizeof(struct my_hook_data));

	h = malloc(sizeof(struct spa_hook));
	spa_hook_list_prepend(&hl, h, &callbacks[0], &data);
	h->removed = hook_removed_cb;

	/* call only the first hook - this should be callback_1 */
	count = spa_hook_list_call_once(&hl, struct my_hook, invoke, HOOK_VERSION);
	pwtest_int_eq(count, 1);
	pwtest_bool_eq(data.cb1, true);
	pwtest_bool_eq(data.cb2, false);
	pwtest_bool_eq(data.cb3, false);

	/* reset cb* booleans to false */
	memset(&data, 0, sizeof(struct my_hook_data));

	/* add callback_4 - this is version 1, so it shouldn't be executed */
	h = malloc(sizeof(struct spa_hook));
	spa_hook_list_append(&hl, h, &callbacks[3], &data);
	h->removed = hook_removed_cb;

	count = spa_hook_list_call(&hl, struct my_hook, invoke, HOOK_VERSION);
	pwtest_int_eq(count, 3);
	pwtest_bool_eq(data.cb1, true);
	pwtest_bool_eq(data.cb2, true);
	pwtest_bool_eq(data.cb3, true);

	count = 0;
	hook_free_count = 0;
	spa_list_consume(h, &hl.list, link) {
		spa_hook_remove(h);
		count++;
	}
	pwtest_int_eq(count, 4);
	pwtest_int_eq(hook_free_count, 4);

	/* remove a zeroed hook */
	struct spa_hook hook;
	spa_zero(hook);
	spa_hook_remove(&hook);

	return PWTEST_PASS;
}

PWTEST(utils_ringbuffer)
{
	struct spa_ringbuffer rb;
	char buffer[20];
	char readbuf[20];
	uint32_t idx;
	int32_t fill;

	spa_ringbuffer_init(&rb);
	fill = spa_ringbuffer_get_write_index(&rb, &idx);
	pwtest_int_eq(idx, 0U);
	pwtest_int_eq(fill, 0);

	spa_ringbuffer_write_data(&rb, buffer, 20, idx, "hello pipewire", 14);
	spa_ringbuffer_write_update(&rb, idx + 14);

	fill = spa_ringbuffer_get_write_index(&rb, &idx);
	pwtest_int_eq(idx, 14U);
	pwtest_int_eq(fill, 14);
	fill = spa_ringbuffer_get_read_index(&rb, &idx);
	pwtest_int_eq(idx, 0U);
	pwtest_int_eq(fill, 14);

	spa_ringbuffer_read_data(&rb, buffer, 20, idx, readbuf, 6);
	spa_ringbuffer_read_update(&rb, idx + 6);
	pwtest_int_eq(memcmp(readbuf, "hello ", 6), 0);

	fill = spa_ringbuffer_get_read_index(&rb, &idx);
	pwtest_int_eq(idx, 6U);
	pwtest_int_eq(fill, 8);
	fill = spa_ringbuffer_get_write_index(&rb, &idx);
	pwtest_int_eq(idx, 14U);
	pwtest_int_eq(fill, 8);

	spa_ringbuffer_write_data(&rb, buffer, 20, idx, " rocks !!!", 10);
	spa_ringbuffer_write_update(&rb, idx + 10);

	fill = spa_ringbuffer_get_write_index(&rb, &idx);
	pwtest_int_eq(idx, 24U);
	pwtest_int_eq(fill, 18);
	fill = spa_ringbuffer_get_read_index(&rb, &idx);
	pwtest_int_eq(idx, 6U);
	pwtest_int_eq(fill, 18);

	spa_ringbuffer_read_data(&rb, buffer, 20, idx, readbuf, 18);
	spa_ringbuffer_read_update(&rb, idx + 18);
	pwtest_str_eq_n(readbuf, "pipewire rocks !!!", 18);

	fill = spa_ringbuffer_get_read_index(&rb, &idx);
	pwtest_int_eq(idx, 24U);
	pwtest_int_eq(fill, 0);
	fill = spa_ringbuffer_get_write_index(&rb, &idx);
	pwtest_int_eq(idx, 24U);
	pwtest_int_eq(fill, 0);

	/* actual buffer must have wrapped around */
	pwtest_str_eq_n(buffer, " !!!o pipewire rocks", 20);
	return PWTEST_PASS;
}

PWTEST(utils_strtol)
{
	int32_t v = 0xabcd;

	pwtest_bool_true(spa_atoi32("0", &v, 0));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi32("0", &v, 16));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi32("0", &v, 32));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi32("-1", &v, 0));		pwtest_int_eq(v, -1);
	pwtest_bool_true(spa_atoi32("-1234", &v, 0));		pwtest_int_eq(v, -1234);
	pwtest_bool_true(spa_atoi32("-2147483648", &v, 0));	pwtest_int_eq(v, -2147483648);
	pwtest_bool_true(spa_atoi32("+1", &v, 0));		pwtest_int_eq(v, 1);
	pwtest_bool_true(spa_atoi32("+1234", &v, 0));		pwtest_int_eq(v, 1234);
	pwtest_bool_true(spa_atoi32("+2147483647", &v, 0));	pwtest_int_eq(v, 2147483647);
	pwtest_bool_true(spa_atoi32("65535", &v, 0));		pwtest_int_eq(v, 0xffff);
	pwtest_bool_true(spa_atoi32("65535", &v, 10));		pwtest_int_eq(v, 0xffff);
	pwtest_bool_true(spa_atoi32("65535", &v, 16));		pwtest_int_eq(v, 0x65535);
	pwtest_bool_true(spa_atoi32("0xff", &v, 0));		pwtest_int_eq(v, 0xff);
	pwtest_bool_true(spa_atoi32("0xff", &v, 16));		pwtest_int_eq(v, 0xff);

	v = 0xabcd;
	pwtest_bool_false(spa_atoi32("0xff", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("fabc", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("fabc", &v, 0));		pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi32("124bogus", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("124bogus", &v, 10));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("124bogus", &v, 16));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("0xbogus", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("bogus", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("bogus", &v, 16));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("", &v, 16));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("  ", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32(" ", &v, 0));		pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi32("-2147483649", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("2147483648", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("9223372036854775807", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("-9223372036854775808", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32("9223372036854775808999", &v, 0));		pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi32(NULL, &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32(NULL, &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi32(NULL, &v, 16));		pwtest_int_eq(v, 0xabcd);

	return PWTEST_PASS;
}

PWTEST(utils_strtoul)
{
	uint32_t v = 0xabcd;

	pwtest_bool_true(spa_atou32("0", &v, 0));		pwtest_int_eq(v, 0U);
	pwtest_bool_true(spa_atou32("0", &v, 16));		pwtest_int_eq(v, 0U);
	pwtest_bool_true(spa_atou32("0", &v, 32));		pwtest_int_eq(v, 0U);
	pwtest_bool_true(spa_atou32("+1", &v, 0));		pwtest_int_eq(v, 1U);
	pwtest_bool_true(spa_atou32("+1234", &v, 0));		pwtest_int_eq(v, 1234U);
	pwtest_bool_true(spa_atou32("+4294967295", &v, 0));	pwtest_int_eq(v, 4294967295U);
	pwtest_bool_true(spa_atou32("4294967295", &v, 0));	pwtest_int_eq(v, 4294967295U);
	pwtest_bool_true(spa_atou32("65535", &v, 0));		pwtest_int_eq(v, 0xffffU);
	pwtest_bool_true(spa_atou32("65535", &v, 10));		pwtest_int_eq(v, 0xffffU);
	pwtest_bool_true(spa_atou32("65535", &v, 16));		pwtest_int_eq(v, 0x65535U);
	pwtest_bool_true(spa_atou32("0xff", &v, 0));		pwtest_int_eq(v, 0xffU);
	pwtest_bool_true(spa_atou32("0xff", &v, 16));		pwtest_int_eq(v, 0xffU);

	v = 0xabcd;
	pwtest_bool_false(spa_atou32("-1", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("-1234", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("-2147483648", &v, 0));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("0xff", &v, 10));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("fabc", &v, 10));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("fabc", &v, 0));		pwtest_int_eq(v, 0xabcdU);

	pwtest_bool_false(spa_atou32("124bogus", &v, 0));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("124bogus", &v, 10));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("124bogus", &v, 16));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("0xbogus", &v, 0));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("bogus", &v, 10));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("bogus", &v, 16));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("", &v, 10));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("", &v, 16));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("  ", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32(" ", &v, 0));		pwtest_int_eq(v, 0xabcdU);

	pwtest_bool_false(spa_atou32("-2147483649", &v, 0));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("4294967296", &v, 0));	pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("9223372036854775807", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("-9223372036854775808", &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32("9223372036854775808999", &v, 0));		pwtest_int_eq(v, 0xabcdU);

	pwtest_bool_false(spa_atou32(NULL, &v, 0));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32(NULL, &v, 10));		pwtest_int_eq(v, 0xabcdU);
	pwtest_bool_false(spa_atou32(NULL, &v, 16));		pwtest_int_eq(v, 0xabcdU);

	return PWTEST_PASS;
}

PWTEST(utils_strtoll)
{
	int64_t v = 0xabcd;

	pwtest_bool_true(spa_atoi64("0", &v, 0));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi64("0", &v, 16));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi64("0", &v, 32));		pwtest_int_eq(v, 0);
	pwtest_bool_true(spa_atoi64("-1", &v, 0));		pwtest_int_eq(v, -1);
	pwtest_bool_true(spa_atoi64("-1234", &v, 0));		pwtest_int_eq(v, -1234);
	pwtest_bool_true(spa_atoi64("-2147483648", &v, 0));	pwtest_int_eq(v, -2147483648);
	pwtest_bool_true(spa_atoi64("+1", &v, 0));		pwtest_int_eq(v, 1);
	pwtest_bool_true(spa_atoi64("+1234", &v, 0));		pwtest_int_eq(v, 1234);
	pwtest_bool_true(spa_atoi64("+2147483647", &v, 0));	pwtest_int_eq(v, 2147483647);
	pwtest_bool_true(spa_atoi64("65535", &v, 0));		pwtest_int_eq(v, 0xffff);
	pwtest_bool_true(spa_atoi64("65535", &v, 10));		pwtest_int_eq(v, 0xffff);
	pwtest_bool_true(spa_atoi64("65535", &v, 16));		pwtest_int_eq(v, 0x65535);
	pwtest_bool_true(spa_atoi64("0xff", &v, 0));		pwtest_int_eq(v, 0xff);
	pwtest_bool_true(spa_atoi64("0xff", &v, 16));		pwtest_int_eq(v, 0xff);
	pwtest_bool_true(spa_atoi64("9223372036854775807", &v, 0));	pwtest_int_eq(v, 0x7fffffffffffffff);
	pwtest_bool_true(spa_atoi64("-9223372036854775808", &v, 0));	pwtest_int_eq((uint64_t)v, 0x8000000000000000);

	v = 0xabcd;
	pwtest_bool_false(spa_atoi64("0xff", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("fabc", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("fabc", &v, 0));		pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi64("124bogus", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("124bogus", &v, 10));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("124bogus", &v, 16));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("0xbogus", &v, 0));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("bogus", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("bogus", &v, 16));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("", &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("", &v, 16));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64("  ", &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64(" ", &v, 0));		pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi64("9223372036854775808999", &v, 0));	pwtest_int_eq(v, 0xabcd);

	pwtest_bool_false(spa_atoi64(NULL, &v, 0));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64(NULL, &v, 10));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atoi64(NULL, &v, 16));		pwtest_int_eq(v, 0xabcd);

	return PWTEST_PASS;
}

PWTEST(utils_strtof)
{
	float v = 0xabcd;

	setlocale(LC_NUMERIC, "C"); /* For decimal number parsing */

	pwtest_bool_true(spa_atof("0", &v));	pwtest_double_eq(v, 0.0f);
	pwtest_bool_true(spa_atof("0.00", &v));	pwtest_double_eq(v, 0.0f);
	pwtest_bool_true(spa_atof("1", &v));	pwtest_double_eq(v, 1.0f);
	pwtest_bool_true(spa_atof("-1", &v));	pwtest_double_eq(v, -1.0f);
	pwtest_bool_true(spa_atof("0x1", &v));	pwtest_double_eq(v, 1.0f);

	v = 0xabcd;
	pwtest_bool_false(spa_atof("0,00", &v));  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof("fabc", &v));  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof("1.bogus", &v));pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof("1.0a", &v));  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof("  ", &v));	  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof(" ", &v));	  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof("", &v));	  pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atof(NULL, &v));	  pwtest_int_eq(v, 0xabcd);

	return PWTEST_PASS;
}

PWTEST(utils_strtod)
{
	double v = 0xabcd;

	pwtest_bool_true(spa_atod("0", &v));		pwtest_double_eq(v, 0.0);
	pwtest_bool_true(spa_atod("0.00", &v));		pwtest_double_eq(v, 0.0);
	pwtest_bool_true(spa_atod("1", &v));		pwtest_double_eq(v, 1.0);
	pwtest_bool_true(spa_atod("-1", &v));		pwtest_double_eq(v, -1.0);
	pwtest_bool_true(spa_atod("0x1", &v));		pwtest_double_eq(v, 1.0);

	v = 0xabcd;
	pwtest_bool_false(spa_atod("0,00", &v));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod("fabc", &v));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod("1.bogus", &v));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod("1.0a", &v));	pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod("  ", &v));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod(" ", &v));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod("", &v));		pwtest_int_eq(v, 0xabcd);
	pwtest_bool_false(spa_atod(NULL, &v));		pwtest_int_eq(v, 0xabcd);

	return PWTEST_PASS;
}

PWTEST(utils_streq)
{
	pwtest_bool_true(spa_streq(NULL, NULL));
	pwtest_bool_true(spa_streq("", ""));
	pwtest_bool_true(spa_streq("a", "a"));
	pwtest_bool_true(spa_streq("abc", "abc"));
	pwtest_bool_false(spa_streq(NULL, "abc"));
	pwtest_bool_false(spa_streq("abc", NULL));

	pwtest_bool_true(spa_strneq("abc", "aaa", 1));
	pwtest_bool_true(spa_strneq("abc", "abc", 7));
	pwtest_bool_false(spa_strneq("abc", "aaa", 2));
	pwtest_bool_false(spa_strneq("abc", NULL, 7));
	pwtest_bool_false(spa_strneq(NULL, "abc", 7));

	return PWTEST_PASS;
}

PWTEST(utils_strendswith)
{
	pwtest_bool_true(spa_strendswith("foo", "o"));
	pwtest_bool_true(spa_strendswith("foobar", "bar"));

	pwtest_bool_false(spa_strendswith(NULL, "bar"));
	pwtest_bool_false(spa_strendswith("foo", "f"));
	pwtest_bool_false(spa_strendswith("foo", "fo"));
	pwtest_bool_false(spa_strendswith("foo", "foobar"));

	return PWTEST_PASS;
}

PWTEST(utils_strendswith_null_suffix)
{
	spa_strendswith("foo", NULL);

	return PWTEST_FAIL;
}

PWTEST(utils_atob)
{
	pwtest_bool_true(spa_atob("true"));
	pwtest_bool_true(spa_atob("1"));
	pwtest_bool_false(spa_atob("0"));
	pwtest_bool_false(spa_atob("-1"));
	pwtest_bool_false(spa_atob("10"));
	pwtest_bool_false(spa_atob("11"));
	pwtest_bool_false(spa_atob("t"));
	pwtest_bool_false(spa_atob("yes"));
	pwtest_bool_false(spa_atob("no"));
	pwtest_bool_false(spa_atob(NULL));
	pwtest_bool_false(spa_atob("True")); /* lower-case required */
	pwtest_bool_false(spa_atob("TRUE"));

	return PWTEST_PASS;
}

PWTEST(utils_ansi)
{
	/* Visual test only */
	printf("%sBOLD%s\n", SPA_ANSI_BOLD, SPA_ANSI_RESET);
	printf("%sUNDERLINE%s\n", SPA_ANSI_UNDERLINE, SPA_ANSI_RESET);
	printf("%sITALIC%s\n", SPA_ANSI_ITALIC, SPA_ANSI_RESET);

	printf("%sBLACK%s\n", SPA_ANSI_BLACK, SPA_ANSI_RESET);
	printf("%sBRIGHT_BLACK%s\n", SPA_ANSI_BRIGHT_BLACK, SPA_ANSI_RESET);
	printf("%sDARK_BLACK%s\n", SPA_ANSI_DARK_BLACK, SPA_ANSI_RESET);
	printf("%sBOLD_BLACK%s\n", SPA_ANSI_BOLD_BLACK, SPA_ANSI_RESET);

	printf("%sRED%s\n", SPA_ANSI_RED, SPA_ANSI_RESET);
	printf("%sBRIGHT_RED%s\n", SPA_ANSI_BRIGHT_RED, SPA_ANSI_RESET);
	printf("%sDARK_RED%s\n", SPA_ANSI_DARK_RED, SPA_ANSI_RESET);
	printf("%sBOLD_RED%s\n", SPA_ANSI_BOLD_RED, SPA_ANSI_RESET);

	printf("%sGREEN%s\n", SPA_ANSI_GREEN, SPA_ANSI_RESET);
	printf("%sBRIGHT_GREEN%s\n", SPA_ANSI_BRIGHT_GREEN, SPA_ANSI_RESET);
	printf("%sDARK_GREEN%s\n", SPA_ANSI_DARK_GREEN, SPA_ANSI_RESET);
	printf("%sBOLD_GREEN%s\n", SPA_ANSI_BOLD_GREEN, SPA_ANSI_RESET);

	printf("%sYELLOW%s\n", SPA_ANSI_YELLOW, SPA_ANSI_RESET);
	printf("%sBRIGHT_YELLOW%s\n", SPA_ANSI_BRIGHT_YELLOW, SPA_ANSI_RESET);
	printf("%sDARK_YELLOW%s\n", SPA_ANSI_DARK_YELLOW, SPA_ANSI_RESET);
	printf("%sBOLD_YELLOW%s\n", SPA_ANSI_BOLD_YELLOW, SPA_ANSI_RESET);

	printf("%sBLUE%s\n", SPA_ANSI_BLUE, SPA_ANSI_RESET);
	printf("%sBRIGHT_BLUE%s\n", SPA_ANSI_BRIGHT_BLUE, SPA_ANSI_RESET);
	printf("%sDARK_BLUE%s\n", SPA_ANSI_DARK_BLUE, SPA_ANSI_RESET);
	printf("%sBOLD_BLUE%s\n", SPA_ANSI_BOLD_BLUE, SPA_ANSI_RESET);

	printf("%sMAGENTA%s\n", SPA_ANSI_MAGENTA, SPA_ANSI_RESET);
	printf("%sBRIGHT_MAGENTA%s\n", SPA_ANSI_BRIGHT_MAGENTA, SPA_ANSI_RESET);
	printf("%sDARK_MAGENTA%s\n", SPA_ANSI_DARK_MAGENTA, SPA_ANSI_RESET);
	printf("%sBOLD_MAGENTA%s\n", SPA_ANSI_BOLD_MAGENTA, SPA_ANSI_RESET);

	printf("%sCYAN%s\n", SPA_ANSI_CYAN, SPA_ANSI_RESET);
	printf("%sBRIGHT_CYAN%s\n", SPA_ANSI_BRIGHT_CYAN, SPA_ANSI_RESET);
	printf("%sDARK_CYAN%s\n", SPA_ANSI_DARK_CYAN, SPA_ANSI_RESET);
	printf("%sBOLD_CYAN%s\n", SPA_ANSI_BOLD_CYAN, SPA_ANSI_RESET);

	printf("%sWHITE%s\n", SPA_ANSI_WHITE, SPA_ANSI_RESET);
	printf("%sBRIGHT_WHITE%s\n", SPA_ANSI_BRIGHT_WHITE, SPA_ANSI_RESET);
	printf("%sDARK_WHITE%s\n", SPA_ANSI_DARK_WHITE, SPA_ANSI_RESET);
	printf("%sBOLD_WHITE%s\n", SPA_ANSI_BOLD_WHITE, SPA_ANSI_RESET);


	/* Background colors */

	printf("%sBG_BLACK%s\n", SPA_ANSI_BG_BLACK, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_BLACK%s\n", SPA_ANSI_BG_BRIGHT_BLACK, SPA_ANSI_RESET);

	printf("%sBG_RED%s\n", SPA_ANSI_BG_RED, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_RED%s\n", SPA_ANSI_BG_BRIGHT_RED, SPA_ANSI_RESET);

	printf("%sBG_GREEN%s\n", SPA_ANSI_BG_GREEN, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_GREEN%s\n", SPA_ANSI_BG_BRIGHT_GREEN, SPA_ANSI_RESET);

	printf("%sBG_YELLOW%s\n", SPA_ANSI_BG_YELLOW, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_YELLOW%s\n", SPA_ANSI_BG_BRIGHT_YELLOW, SPA_ANSI_RESET);

	printf("%sBG_BLUE%s\n", SPA_ANSI_BG_BLUE, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_BLUE%s\n", SPA_ANSI_BG_BRIGHT_BLUE, SPA_ANSI_RESET);

	printf("%sBG_MAGENTA%s\n", SPA_ANSI_BG_MAGENTA, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_MAGENTA%s\n", SPA_ANSI_BG_BRIGHT_MAGENTA, SPA_ANSI_RESET);

	printf("%sBG_CYAN%s\n", SPA_ANSI_BG_CYAN, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_CYAN%s\n", SPA_ANSI_BG_BRIGHT_CYAN, SPA_ANSI_RESET);

	printf("%sBG_WHITE%s\n", SPA_ANSI_BG_WHITE, SPA_ANSI_RESET);
	printf("%sBG_BRIGHT_WHITE%s\n", SPA_ANSI_BG_BRIGHT_WHITE, SPA_ANSI_RESET);

	/* A combo */
	printf("normal%s%s%sBG_BLUE,ITALIC,BOLD_YELLOW%snormal\n", SPA_ANSI_BG_BLUE,
	       SPA_ANSI_ITALIC, SPA_ANSI_BOLD_YELLOW, SPA_ANSI_RESET);
	return PWTEST_PASS;
}

PWTEST(utils_snprintf)
{
	char dest[8];
	int len;

	/* Basic printf */
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "foo%d%s", 10, "2"), 6);
	pwtest_str_eq(dest, "foo102");
	/* Print a few strings, make sure dest is truncated and return value
	 * is the length of the returned string */
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "1234567"), 7);
	pwtest_str_eq(dest, "1234567");
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "12345678"), 7);
	pwtest_str_eq(dest, "1234567");
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "123456789"), 7);
	pwtest_str_eq(dest, "1234567");
	/* Same as above, but with printf %s expansion */
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "%s", "1234567"), 7);
	pwtest_str_eq(dest, "1234567");
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "%s", "12345678"), 7);
	pwtest_str_eq(dest, "1234567");
	pwtest_int_eq(spa_scnprintf(dest, sizeof(dest), "%s", "123456789"), 7);
	pwtest_str_eq(dest, "1234567");

	pwtest_int_eq(spa_scnprintf(dest, 2, "1234567"), 1);
	pwtest_str_eq(dest, "1");
	pwtest_int_eq(spa_scnprintf(dest, 1, "1234567"), 0);
	pwtest_str_eq(dest, "");

	/* The "append until buffer is full" use-case */
	len = 0;
	while ((size_t)len < sizeof(dest) - 1)
		len += spa_scnprintf(dest + len, sizeof(dest) - len, "123");
	/* and once more for good measure, this should print 0 characters */
	len = spa_scnprintf(dest + len, sizeof(dest) - len, "abc");
	pwtest_int_eq(len, 0);
	pwtest_str_eq(dest, "1231231");

	return PWTEST_PASS;
}

PWTEST(utils_snprintf_abort_neg_size)
{
	size_t size = pwtest_get_iteration(current_test);
	char dest[8];

	if (RUNNING_ON_VALGRIND)
		return PWTEST_SKIP;

	spa_scnprintf(dest, size, "1234"); /* expected to abort() */

	return PWTEST_FAIL;
}

struct cbtest_data {
	bool invoked;
	const char *data;
};

static void cbtest_func(void *object, const char *msg)
{
	struct cbtest_data *data = object;
	data->invoked = true;
	data->data = msg;
}

PWTEST(utils_callback)
{
	struct cbtest_methods {
		uint32_t version;
		void (*func_v0)(void *object, const char *msg);
		void (*func_v1)(void *object, const char *msg);
	} methods = { 0, cbtest_func, cbtest_func };
	struct cbtest {
		struct spa_interface iface;
	} cbtest;
	struct cbtest_data data;

	/* Interface version doesn't matter for this test */
	cbtest.iface = SPA_INTERFACE_INIT("cbtest type", 0, &methods, &data);

	/* Methods are version 0 */
	methods.version = 0;
	data.invoked = false;
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v0, 0, "cbtest v0");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.data, "cbtest v0");

	/* v1 call should be silently filtered */
	data.invoked = false;
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v1, 1, "cbtest v1");
	pwtest_bool_false(data.invoked);

	/* Methods are version 1 */
	methods.version = 1;
	data.invoked = false;
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v0, 0, "cbtest v0");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.data, "cbtest v0");

	/* v1 call expected to be called */
	data.invoked = false;
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v1, 1, "cbtest v1");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.data, "cbtest v1");

	return PWTEST_PASS;
}

PWTEST(utils_callback_func_is_null)
{
	struct cbtest_methods {
		uint32_t version;
		void (*func_v0)(void *object, const char *msg);
		void (*func_v1)(void *object, const char *msg);
	} methods = { 0, NULL, NULL };
	struct cbtest {
		struct spa_interface iface;
	} cbtest;
	struct cbtest_data data;

	/* Interface version doesn't matter for this test */
	cbtest.iface = SPA_INTERFACE_INIT("cbtest type", 0, &methods, &data);

	/* Methods are version 0 */
	methods.version = 0;

	/* func_v0 and func_v1 are NULL so this shouldn't crash */
	data.invoked = false;
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v0, 0, "cbtest v0");
	pwtest_bool_false(data.invoked);
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v1, 0, "cbtest v1");
	pwtest_bool_false(data.invoked);

	/* func_v1 is NULL so this shouldn't crash, though the call should get
	 * filtered anyway due to version mismatch */
	spa_interface_call(&cbtest.iface,
			   struct cbtest_methods,
			   func_v1, 1, "cbtest v1");
	pwtest_bool_false(data.invoked);

	return PWTEST_PASS;
}

PWTEST(utils_callback_version)
{
	struct cbtest_methods {
		uint32_t version;
		void (*func_v0)(void *object, const char *msg);
	} methods = { 0, cbtest_func };
	struct cbtest {
		struct spa_interface iface;
	} cbtest;
	struct cbtest_data data;

	/* Interface version doesn't matter for this test */
	cbtest.iface = SPA_INTERFACE_INIT("cbtest type", 0, &methods, &data);

	/* Methods are version 0 */
	methods.version = 0;
	pwtest_bool_true(spa_interface_callback_version_min(&cbtest.iface,
							    struct cbtest_methods,
							    0));
	pwtest_bool_false(spa_interface_callback_version_min(&cbtest.iface,
							     struct cbtest_methods,
							     1));
	/* Methods are version 1 */
	methods.version = 1;
	pwtest_bool_true(spa_interface_callback_version_min(&cbtest.iface,
							    struct cbtest_methods,
							    0));
	pwtest_bool_true(spa_interface_callback_version_min(&cbtest.iface,
							    struct cbtest_methods,
							    1));
	pwtest_bool_false(spa_interface_callback_version_min(&cbtest.iface,
							     struct cbtest_methods,
							     2));

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_utils)
{
	pwtest_add(utils_abi_sizes, PWTEST_NOARG);
	pwtest_add(utils_abi, PWTEST_NOARG);
	pwtest_add(utils_macros, PWTEST_NOARG);
	pwtest_add(utils_result, PWTEST_NOARG);
	pwtest_add(utils_dict, PWTEST_NOARG);
	pwtest_add(utils_list, PWTEST_NOARG);
	pwtest_add(utils_hook, PWTEST_NOARG);
	pwtest_add(utils_ringbuffer, PWTEST_NOARG);
	pwtest_add(utils_strtol, PWTEST_NOARG);
	pwtest_add(utils_strtoul, PWTEST_NOARG);
	pwtest_add(utils_strtoll, PWTEST_NOARG);
	pwtest_add(utils_strtof, PWTEST_NOARG);
	pwtest_add(utils_strtod, PWTEST_NOARG);
	pwtest_add(utils_streq, PWTEST_NOARG);
	pwtest_add(utils_strendswith, PWTEST_NOARG);
	pwtest_add(utils_strendswith_null_suffix,
		   PWTEST_ARG_SIGNAL, SIGABRT);
	pwtest_add(utils_snprintf, PWTEST_NOARG);
	pwtest_add(utils_snprintf_abort_neg_size,
		   PWTEST_ARG_SIGNAL, SIGABRT,
		   PWTEST_ARG_RANGE, -2, 0);
	pwtest_add(utils_atob, PWTEST_NOARG);
	pwtest_add(utils_ansi, PWTEST_NOARG);
	pwtest_add(utils_callback, PWTEST_NOARG);
	pwtest_add(utils_callback_func_is_null, PWTEST_NOARG);
	pwtest_add(utils_callback_version, PWTEST_NOARG);

	return PWTEST_PASS;
}
