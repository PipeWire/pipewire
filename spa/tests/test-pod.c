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

		spa_assert(SPA_POD_SIZE(&pod) == sizeof(int64_t) + 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == sizeof(int64_t));
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == sizeof(int64_t));

		pod = SPA_POD_INIT(sizeof(int32_t), SPA_TYPE_Int);
		spa_assert(SPA_POD_SIZE(&pod) == sizeof(int32_t) + 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == sizeof(int32_t));
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == sizeof(int32_t));
	}
	{
		struct spa_pod pod = SPA_POD_INIT_None();

		spa_assert(SPA_POD_SIZE(&pod) == 8);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_None);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 0);
		spa_assert(SPA_POD_CONTENTS_SIZE(struct spa_pod, &pod) == 0);
	}
	{
		struct spa_pod_bool pod = SPA_POD_INIT_Bool(true);

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Bool);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_bool, &pod) == true);

		pod = SPA_POD_INIT_Bool(false);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Bool);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_bool, &pod) == false);
	}
	{
		struct spa_pod_id pod = SPA_POD_INIT_Id(SPA_TYPE_Int);

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Id);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_id, &pod) == SPA_TYPE_Int);

		pod = SPA_POD_INIT_Id(SPA_TYPE_Long);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Id);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_id, &pod) == SPA_TYPE_Long);
	}
	{
		struct spa_pod_int pod = SPA_POD_INIT_Int(23);

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_int, &pod) == 23);

		pod = SPA_POD_INIT_Int(-123);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Int);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_int, &pod) == -123);
	}
	{
		struct spa_pod_long pod = SPA_POD_INIT_Long(-23);

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_long, &pod) == -23);

		pod = SPA_POD_INIT_Long(123);
		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Long);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_long, &pod) == 123);
	}
	{
		struct spa_pod_float pod = SPA_POD_INIT_Float(0.67f);

		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Float);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_float, &pod) == 0.67f);

		pod = SPA_POD_INIT_Float(134.8f);
		spa_assert(SPA_POD_SIZE(&pod) == 12);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Float);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 4);
		spa_assert(SPA_POD_VALUE(struct spa_pod_float, &pod) == 134.8f);
	}
	{
		struct spa_pod_double pod = SPA_POD_INIT_Double(0.67);

		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Double);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_double, &pod) == 0.67);

		pod = SPA_POD_INIT_Double(134.8);
		spa_assert(SPA_POD_SIZE(&pod) == 16);
		spa_assert(SPA_POD_TYPE(&pod) == SPA_TYPE_Double);
		spa_assert(SPA_POD_BODY_SIZE(&pod) == 8);
		spa_assert(SPA_POD_VALUE(struct spa_pod_double, &pod) == 134.8);
	}
}


int main(int argc, char *argv[])
{
	test_abi();
	test_init();
	return 0;
}
