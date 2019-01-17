/* Simple Plugin API
 * Copyright © 2019 Wim Taymans <wim.taymans@gmail.com>
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

#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/pod/command.h>
#include <spa/pod/event.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

static void test_abi(void)
{
	/* pod */
	spa_assert(sizeof(struct spa_pod) == 8);
	spa_assert(sizeof(struct spa_pod_bool) == 16);
	spa_assert(sizeof(struct spa_pod_id) == 16);
	spa_assert(sizeof(struct spa_pod_int) == 16);
	spa_assert(sizeof(struct spa_pod_long) == 16);
	spa_assert(sizeof(struct spa_pod_float) == 16);
	spa_assert(sizeof(struct spa_pod_double) == 16);
	spa_assert(sizeof(struct spa_pod_string) == 8);
	spa_assert(sizeof(struct spa_pod_bytes) == 8);
	spa_assert(sizeof(struct spa_pod_rectangle) == 16);
	spa_assert(sizeof(struct spa_pod_fraction) == 16);
	spa_assert(sizeof(struct spa_pod_bitmap) == 8);
	spa_assert(sizeof(struct spa_pod_array_body) == 8);
	spa_assert(sizeof(struct spa_pod_array) == 16);

	spa_assert(SPA_CHOICE_None == 0);
	spa_assert(SPA_CHOICE_Range == 1);
	spa_assert(SPA_CHOICE_Step == 2);
	spa_assert(SPA_CHOICE_Enum == 3);
	spa_assert(SPA_CHOICE_Flags == 4);

	spa_assert(sizeof(struct spa_pod_choice_body) == 16);
	spa_assert(sizeof(struct spa_pod_choice) == 24);
	spa_assert(sizeof(struct spa_pod_struct) == 8);
	spa_assert(sizeof(struct spa_pod_object_body) == 8);
	spa_assert(sizeof(struct spa_pod_object) == 16);
	spa_assert(sizeof(struct spa_pod_pointer_body) == 16);
	spa_assert(sizeof(struct spa_pod_pointer) == 24);
	spa_assert(sizeof(struct spa_pod_fd) == 16);
	spa_assert(sizeof(struct spa_pod_prop) == 16);
	spa_assert(sizeof(struct spa_pod_control) == 16);
	spa_assert(sizeof(struct spa_pod_sequence_body) == 8);
	spa_assert(sizeof(struct spa_pod_sequence) == 16);

	/* builder */
	spa_assert(sizeof(struct spa_pod_frame) == 16);
	spa_assert(sizeof(struct spa_pod_builder_state) == 16);
	spa_assert(sizeof(struct spa_pod_builder) == 312);

	/* command */
	spa_assert(sizeof(struct spa_command_body) == 8);
	spa_assert(sizeof(struct spa_command) == 16);

	/* event */
	spa_assert(sizeof(struct spa_event_body) == 8);
	spa_assert(sizeof(struct spa_event) == 16);

	/* iter */
	spa_assert(sizeof(struct spa_pod_iter) == 16);

	/* parser */
	spa_assert(sizeof(struct spa_pod_parser) == 264);

}

static void test_init(void)
{
	{
		struct spa_pod pod = SPA_POD_INIT(sizeof(int64_t), SPA_TYPE_Long);
		int32_t val;

		spa_assert(SPA_POD_SIZE(&pod) == sizeof(int64_t) + 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == sizeof(int64_t));
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == sizeof(int64_t));
		spa_assert(spa_pod_is_long(&pod));

		pod = SPA_POD_INIT(sizeof(int32_t), SPA_TYPE_Int);
		spa_assert(SPA_POD_SIZE(&pod) == sizeof(int32_t) + 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == sizeof(int32_t));
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == sizeof(int32_t));
		spa_assert(spa_pod_is_int(&pod));

		/** too small */
		pod = SPA_POD_INIT(0, SPA_TYPE_Int);
		spa_assert(!spa_pod_is_int(&pod));
		spa_assert(spa_pod_get_int(&pod, &val) < 0);
	}
	{
		struct spa_pod pod = SPA_POD_INIT_None();

		spa_assert(SPA_POD_SIZE(&pod) == 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_None);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 0);
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == 0);
		spa_assert(spa_pod_is_none(&pod));
	}
	{
		struct spa_pod_bool pod = SPA_POD_INIT_Bool(true);
		bool val;

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Bool);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_bool, &pod) == true);
		spa_assert(spa_pod_is_bool(&pod.pod));
		spa_assert(spa_pod_get_bool(&pod.pod, &val) == 0);
		spa_assert(val == true);

		pod = SPA_POD_INIT_Bool(false);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Bool);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_bool, &pod) == false);
		spa_assert(spa_pod_is_bool(&pod.pod));
		spa_assert(spa_pod_get_bool(&pod.pod, &val) == 0);
		spa_assert(val == false);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Bool);
		spa_assert(!spa_pod_is_bool(&pod.pod));
		spa_assert(spa_pod_get_bool(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_id pod = SPA_POD_INIT_Id(SPA_TYPE_Int);
		uint32_t val;

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Id);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_id, &pod) == SPA_TYPE_Int);
		spa_assert(spa_pod_is_id(&pod.pod));
		spa_assert(spa_pod_get_id(&pod.pod, &val) == 0);
		spa_assert(val == SPA_TYPE_Int);

		pod = SPA_POD_INIT_Id(SPA_TYPE_Long);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Id);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_id, &pod) == SPA_TYPE_Long);
		spa_assert(spa_pod_is_id(&pod.pod));
		spa_assert(spa_pod_get_id(&pod.pod, &val) == 0);
		spa_assert(val == SPA_TYPE_Long);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Id);
		spa_assert(!spa_pod_is_id(&pod.pod));
		spa_assert(spa_pod_get_id(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_int pod = SPA_POD_INIT_Int(23);
		int32_t val;

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_int, &pod) == 23);
		spa_assert(spa_pod_is_int(&pod.pod));
		spa_assert(spa_pod_get_int(&pod.pod, &val) == 0);
		spa_assert(val == 23);

		pod = SPA_POD_INIT_Int(-123);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_int, &pod) == -123);
		spa_assert(spa_pod_is_int(&pod.pod));
		spa_assert(spa_pod_get_int(&pod.pod, &val) == 0);
		spa_assert(val == -123);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Int);
		spa_assert(!spa_pod_is_int(&pod.pod));
		spa_assert(spa_pod_get_int(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_long pod = SPA_POD_INIT_Long(-23);
		int64_t val;

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_long, &pod) == -23);
		spa_assert(spa_pod_is_long(&pod.pod));
		spa_assert(spa_pod_get_long(&pod.pod, &val) == 0);
		spa_assert(val == -23);

		pod = SPA_POD_INIT_Long(123);
		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_long, &pod) == 123);
		spa_assert(spa_pod_is_long(&pod.pod));
		spa_assert(spa_pod_get_long(&pod.pod, &val) == 0);
		spa_assert(val == 123);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Long);
		spa_assert(!spa_pod_is_long(&pod.pod));
		spa_assert(spa_pod_get_long(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_float pod = SPA_POD_INIT_Float(0.67f);
		float val;

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Float);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_float, &pod) == 0.67f);
		spa_assert(spa_pod_is_float(&pod.pod));
		spa_assert(spa_pod_get_float(&pod.pod, &val) == 0);
		spa_assert(val == 0.67f);

		pod = SPA_POD_INIT_Float(-134.8f);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Float);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_float, &pod) == -134.8f);
		spa_assert(spa_pod_is_float(&pod.pod));
		spa_assert(spa_pod_get_float(&pod.pod, &val) == 0);
		spa_assert(val == -134.8f);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Float);
		spa_assert(!spa_pod_is_float(&pod.pod));
		spa_assert(spa_pod_get_float(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_double pod = SPA_POD_INIT_Double(0.67);
		double val;

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Double);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_double, &pod) == 0.67);
		spa_assert(spa_pod_is_double(&pod.pod));
		spa_assert(spa_pod_get_double(&pod.pod, &val) == 0);
		spa_assert(val == 0.67);

		pod = SPA_POD_INIT_Double(-134.8);
		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Double);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_double, &pod) == -134.8);
		spa_assert(spa_pod_is_double(&pod.pod));
		spa_assert(spa_pod_get_double(&pod.pod, &val) == 0);
		spa_assert(val == -134.8);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Double);
		spa_assert(!spa_pod_is_double(&pod.pod));
		spa_assert(spa_pod_get_double(&pod.pod, &val) < 0);
	}
	{
		struct {
			struct spa_pod_string pod;
			char str[9];
		} pod;
		char val[12];

		pod.pod	= SPA_POD_INIT_String(9);
		strncpy(pod.str, "test", 9);

		spa_assert(SPA_POD_SIZE(&pod) == 17);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_String);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 9);
		spa_assert(spa_pod_is_string(&pod.pod.pod));
		spa_assert(spa_pod_copy_string(&pod.pod.pod, sizeof(val), val) == 0);
		spa_assert(strcmp(pod.str, val) == 0);

		pod.pod	= SPA_POD_INIT_String(6);
		memcpy(pod.str, "test123456789", 9);

		spa_assert(SPA_POD_SIZE(&pod) == 14);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_String);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 6);
		spa_assert(!spa_pod_is_string(&pod.pod.pod));
		spa_assert(spa_pod_copy_string(&pod.pod.pod, sizeof(val), val) < 0);
	}
	{
		struct spa_pod_rectangle pod = SPA_POD_INIT_Rectangle(SPA_RECTANGLE(320,240));
		struct spa_rectangle val;

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Rectangle);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(memcmp(&SPA_POD_VALUE(struct spa_pod_rectangle, &pod),
					&SPA_RECTANGLE(320,240), sizeof(struct spa_rectangle)) == 0);
		spa_assert(spa_pod_is_rectangle(&pod.pod));
		spa_assert(spa_pod_get_rectangle(&pod.pod, &val) == 0);
		spa_assert(memcmp(&val, &SPA_RECTANGLE(320,240), sizeof(struct spa_rectangle)) == 0);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Rectangle);
		spa_assert(!spa_pod_is_rectangle(&pod.pod));
		spa_assert(spa_pod_get_rectangle(&pod.pod, &val) < 0);
	}
	{
		struct spa_pod_fraction pod = SPA_POD_INIT_Fraction(SPA_FRACTION(25,1));
		struct spa_fraction val;

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Fraction);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(memcmp(&SPA_POD_VALUE(struct spa_pod_fraction, &pod),
					&SPA_FRACTION(25,1), sizeof(struct spa_fraction)) == 0);
		spa_assert(spa_pod_is_fraction(&pod.pod));
		spa_assert(spa_pod_get_fraction(&pod.pod, &val) == 0);
		spa_assert(memcmp(&val, &SPA_FRACTION(25,1), sizeof(struct spa_fraction)) == 0);

		pod.pod = SPA_POD_INIT(0, SPA_TYPE_Fraction);
		spa_assert(!spa_pod_is_fraction(&pod.pod));
		spa_assert(spa_pod_get_fraction(&pod.pod, &val) < 0);
	}
}

static void test_build(void)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	struct spa_pod *array, *choice, *head, *pod, *it;
	struct spa_pod_prop *prop;
	struct spa_pod_control *control;
	int64_t longs[] = { 5, 7, 11, 13, 17 }, *al;
	uint32_t i, len, zl, *ai;
	union {
		bool b;
		uint32_t I;
		int32_t i;
		int64_t l;
		float f;
		double d;
		const char *s;
		const void *z;
		const void *p;
		int64_t h;
		struct spa_rectangle R;
		struct spa_fraction F;
	} val;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_assert(b.data == buffer);
	spa_assert(b.size == sizeof(buffer));
	spa_assert(b.state.offset == 0);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_none(&b) == 0);
	spa_assert(spa_pod_builder_bool(&b, true) == 8);
	spa_assert(spa_pod_builder_id(&b, SPA_TYPE_Object) == 24);
	spa_assert(spa_pod_builder_int(&b, 21) == 40);
	spa_assert(spa_pod_builder_float(&b, 0.8f) == 56);
	spa_assert(spa_pod_builder_double(&b, -1.56) == 72);
	spa_assert(spa_pod_builder_string(&b, "test") == 88);
	spa_assert(spa_pod_builder_bytes(&b, "PipeWire", 8) == 104);
	spa_assert(spa_pod_builder_pointer(&b, SPA_TYPE_Object, &b) == 120);
	spa_assert(spa_pod_builder_fd(&b, 4) == 144);
	spa_assert(spa_pod_builder_rectangle(&b, 320, 240) == 160);
	spa_assert(spa_pod_builder_fraction(&b, 25, 1) == 176);

	spa_assert(spa_pod_builder_push_array(&b) == 192);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_BODY | SPA_POD_BUILDER_FLAG_FIRST));
	spa_assert(b.state.depth == 1);
	spa_assert(spa_pod_builder_int(&b, 1) == 200);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_BODY);
	spa_assert(spa_pod_builder_int(&b, 2) == 212);
	spa_assert(spa_pod_builder_int(&b, 3) == 216);
	array = spa_pod_builder_pop(&b);
	spa_assert(array != NULL);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_array(&b,
				sizeof(int64_t), SPA_TYPE_Long,
				SPA_N_ELEMENTS(longs), longs) == 224);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_push_choice(&b, SPA_CHOICE_Enum, 0) == 280);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_BODY | SPA_POD_BUILDER_FLAG_FIRST));
	spa_assert(b.state.depth == 1);
	spa_assert(spa_pod_builder_long(&b, 1) == 296);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_BODY);
	spa_assert(spa_pod_builder_long(&b, 2) == 312);
	spa_assert(spa_pod_builder_long(&b, 3) == 320);
	choice = spa_pod_builder_pop(&b);
	spa_assert(choice != NULL);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_push_struct(&b) == 328);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 1);
	spa_assert(spa_pod_builder_int(&b, 21) == 336);
	spa_assert(spa_pod_builder_float(&b, 0.8f) == 352);
	spa_assert(spa_pod_builder_double(&b, -1.56) == 368);
	spa_assert(spa_pod_builder_pop(&b) != NULL);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_Props, 0) == 384);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_OBJECT);
	spa_assert(b.state.depth == 1);
	spa_assert(spa_pod_builder_prop(&b, 1, 0) == 400);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_OBJECT | SPA_POD_BUILDER_FLAG_HEADER));
	spa_assert(spa_pod_builder_int(&b, 21) == 408);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_OBJECT);
	spa_assert(spa_pod_builder_prop(&b, 2, 0) == 424);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_OBJECT | SPA_POD_BUILDER_FLAG_HEADER));
	spa_assert(spa_pod_builder_long(&b, 42) == 432);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_OBJECT);
	spa_assert(spa_pod_builder_prop(&b, 3, 0) == 448);
	spa_assert(spa_pod_builder_string(&b, "test123") == 456);
	spa_assert(spa_pod_builder_pop(&b) != NULL);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(spa_pod_builder_push_sequence(&b, 0) == 472);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_SEQUENCE);
	spa_assert(b.state.depth == 1);
	spa_assert(spa_pod_builder_control(&b, 0, 0) == 488);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_SEQUENCE | SPA_POD_BUILDER_FLAG_HEADER));
	spa_assert(spa_pod_builder_float(&b, 0.667f) == 496);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_SEQUENCE);
	spa_assert(spa_pod_builder_control(&b, 12, 0) == 512);
	spa_assert(b.state.flags == (SPA_POD_BUILDER_FLAG_SEQUENCE | SPA_POD_BUILDER_FLAG_HEADER));
	spa_assert(spa_pod_builder_double(&b, 1.22) == 520);
	spa_assert(b.state.flags == SPA_POD_BUILDER_FLAG_SEQUENCE);
	spa_assert(spa_pod_builder_pop(&b) != NULL);
	spa_assert(b.state.flags == 0);
	spa_assert(b.state.depth == 0);

	spa_assert(b.state.offset == 536);

	len = b.state.offset;
	pod = head = (struct spa_pod *)buffer;

	spa_assert(spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_none(pod));
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_bool(pod));
	spa_assert(spa_pod_get_bool(pod, &val.b) == 0);
	spa_assert(val.b == true);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_id(pod));
	spa_assert(spa_pod_get_id(pod, &val.I) == 0);
	spa_assert(val.I == SPA_TYPE_Object);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_int(pod));
	spa_assert(spa_pod_get_int(pod, &val.i) == 0);
	spa_assert(val.i == 21);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_float(pod));
	spa_assert(spa_pod_get_float(pod, &val.f) == 0);
	spa_assert(val.f == 0.8f);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_double(pod));
	spa_assert(spa_pod_get_double(pod, &val.d) == 0);
	spa_assert(val.d == -1.56);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_string(pod));
	spa_assert(spa_pod_get_string(pod, &val.s) == 0);
	spa_assert(strcmp(val.s, "test") == 0);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_bytes(pod));
	spa_assert(spa_pod_get_bytes(pod, &val.z, &zl) == 0);
	spa_assert(zl == 8);
	spa_assert(memcmp(val.z, "PipeWire", zl) == 0);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_pointer(pod));
	spa_assert(spa_pod_get_pointer(pod, &zl, &val.p) == 0);
	spa_assert(zl == SPA_TYPE_Object);
	spa_assert(val.p == &b);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_fd(pod));
	spa_assert(spa_pod_get_fd(pod, &val.l) == 0);
	spa_assert(val.l == 4);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_rectangle(pod));
	spa_assert(spa_pod_get_rectangle(pod, &val.R) == 0);
	spa_assert(memcmp(&val.R, &SPA_RECTANGLE(320,240), sizeof(struct spa_rectangle)) == 0);
	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_fraction(pod));
	spa_assert(spa_pod_get_fraction(pod, &val.F) == 0);
	spa_assert(memcmp(&val.F, &SPA_FRACTION(25,1), sizeof(struct spa_fraction)) == 0);

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_array(pod));
	spa_assert(SPA_POD_ARRAY_VALUE_TYPE(pod) == SPA_TYPE_Int);
	spa_assert(SPA_POD_ARRAY_VALUE_SIZE(pod) == sizeof(int32_t));
	spa_assert(SPA_POD_ARRAY_N_VALUES(pod) == 3);
	spa_assert((ai = SPA_POD_ARRAY_VALUES(pod)) != NULL);
	spa_assert(SPA_POD_ARRAY_CHILD(pod)->type == SPA_TYPE_Int);
	spa_assert(SPA_POD_ARRAY_CHILD(pod)->size == sizeof(int32_t));
	spa_assert(ai[0] == 1);
	spa_assert(ai[1] == 2);
	spa_assert(ai[2] == 3);
	i = 1;
	SPA_POD_ARRAY_FOREACH((struct spa_pod_array*)pod, ai) {
		spa_assert(*ai == i);
		i++;
	}

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_array(pod));
	spa_assert(SPA_POD_ARRAY_VALUE_TYPE(pod) == SPA_TYPE_Long);
	spa_assert(SPA_POD_ARRAY_VALUE_SIZE(pod) == sizeof(int64_t));
	spa_assert(SPA_POD_ARRAY_N_VALUES(pod) == SPA_N_ELEMENTS(longs));
	spa_assert((al = SPA_POD_ARRAY_VALUES(pod)) != NULL);
	spa_assert(SPA_POD_ARRAY_CHILD(pod)->type == SPA_TYPE_Long);
	spa_assert(SPA_POD_ARRAY_CHILD(pod)->size == sizeof(int64_t));
	for (i = 0; i < SPA_N_ELEMENTS(longs); i++)
		spa_assert(al[i] == longs[i]);
	i = 0;
	SPA_POD_ARRAY_FOREACH((struct spa_pod_array*)pod, al) {
		spa_assert(*al == longs[i++]);
	}

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_choice(pod));
	spa_assert(SPA_POD_CHOICE_TYPE(pod) == SPA_CHOICE_Enum);
	spa_assert(SPA_POD_CHOICE_FLAGS(pod) == 0);
	spa_assert(SPA_POD_CHOICE_VALUE_TYPE(pod) == SPA_TYPE_Long);
	spa_assert(SPA_POD_CHOICE_VALUE_SIZE(pod) == sizeof(int64_t));
	spa_assert(SPA_POD_CHOICE_N_VALUES(pod) == 3);
	spa_assert((al = SPA_POD_CHOICE_VALUES(pod)) != NULL);
	spa_assert(SPA_POD_CHOICE_CHILD(pod)->type == SPA_TYPE_Long);
	spa_assert(SPA_POD_CHOICE_CHILD(pod)->size == sizeof(int64_t));
	spa_assert(al[0] == 1);
	spa_assert(al[1] == 2);
	spa_assert(al[2] == 3);
	i = 1;
	SPA_POD_CHOICE_FOREACH((struct spa_pod_choice*)pod, al) {
		spa_assert(*al == i);
		i++;
	}

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_struct(pod));
	i = 0;
	SPA_POD_STRUCT_FOREACH(pod, it) {
		switch (i++) {
		case 0:
			spa_assert(spa_pod_is_int(it));
			spa_assert(spa_pod_get_int(it, &val.i) == 0 && val.i == 21);
			break;
		case 1:
			spa_assert(spa_pod_is_float(it));
			spa_assert(spa_pod_get_float(it, &val.f) == 0 && val.f == 0.8f);
			break;
		case 2:
			spa_assert(spa_pod_is_double(it));
			spa_assert(spa_pod_get_double(it, &val.d) == 0 && val.d == -1.56);
			break;
		default:
			spa_assert_not_reached();
			break;
		}
	}

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_object(pod));
	spa_assert(spa_pod_is_object_type(pod, SPA_TYPE_OBJECT_Props));
	spa_assert(spa_pod_is_object_id(pod, 0));
	i = 0;
	SPA_POD_OBJECT_FOREACH((const struct spa_pod_object*)pod, prop) {
		switch (i++) {
		case 0:
			spa_assert(prop->key == 1);
			spa_assert(SPA_POD_PROP_SIZE(prop) == 20);
			spa_assert(spa_pod_get_int(&prop->value, &val.i) == 0 && val.i == 21);
			break;
		case 1:
			spa_assert(prop->key == 2);
			spa_assert(SPA_POD_PROP_SIZE(prop) == 24);
			spa_assert(spa_pod_get_long(&prop->value, &val.l) == 0 && val.l == 42);
			break;
		case 2:
			spa_assert(prop->key == 3);
			spa_assert(SPA_POD_PROP_SIZE(prop) == 24);
			spa_assert(spa_pod_get_string(&prop->value, &val.s) == 0 &&
					strcmp(val.s, "test123") == 0);
			break;
		default:
			spa_assert_not_reached();
			break;
		}
	}
	spa_assert((prop = spa_pod_find_prop(pod, 3)) != NULL);
	spa_assert(prop->key == 3);
	spa_assert(spa_pod_get_string(&prop->value, &val.s) == 0 &&
				strcmp(val.s, "test123") == 0);
	spa_assert((prop = spa_pod_find_prop(pod, 1)) != NULL);
	spa_assert(prop->key == 1);
	spa_assert(spa_pod_get_int(&prop->value, &val.i) == 0 && val.i == 21);
	spa_assert((prop = spa_pod_find_prop(pod, 2)) != NULL);
	spa_assert(prop->key == 2);
	spa_assert(spa_pod_get_long(&prop->value, &val.l) == 0 && val.l == 42);
	spa_assert((prop = spa_pod_find_prop(pod, 5)) == NULL);

	spa_assert((pod = spa_pod_next(pod)) != NULL && spa_pod_is_inside(head, len, pod));
	spa_assert(spa_pod_is_sequence(pod));

	i = 0;
	SPA_POD_SEQUENCE_FOREACH((const struct spa_pod_sequence*)pod, control) {
		switch (i++) {
		case 0:
			spa_assert(control->offset == 0);
			spa_assert(SPA_POD_CONTROL_SIZE(control) == 20);
			spa_assert(spa_pod_get_float(&control->value, &val.f) == 0 && val.f == 0.667f);
			break;
		case 1:
			spa_assert(control->offset == 12);
			spa_assert(SPA_POD_CONTROL_SIZE(control) == 24);
			spa_assert(spa_pod_get_double(&control->value, &val.d) == 0 && val.d == 1.22);
			break;
		default:
			spa_assert_not_reached();
			break;
		}
	}
}


int main(int argc, char *argv[])
{
	test_abi();
	test_init();
	test_build();
	return 0;
}
