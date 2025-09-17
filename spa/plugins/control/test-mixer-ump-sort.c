/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Claude Code */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <spa/control/control.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

/* Include the mixer source to access static inline functions */
#include "mixer.c"

/* Simple test framework macros */
#define TEST_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL: %s\n", msg); \
			return 1; \
		} \
	} while (0)

#define TEST_PASS() \
	do { \
		printf("PASS\n"); \
		return 0; \
	} while (0)

/* Helper to create mock control structures for testing */
static struct spa_pod_control *create_mock_control(uint8_t *buffer, size_t *offset,
		uint64_t timestamp, uint32_t type, const void *data, size_t data_size)
{
	struct spa_pod_control *control = (struct spa_pod_control *)(buffer + *offset);
	control->offset = timestamp;
	control->type = type;
	control->value.size = data_size;
	control->value.type = SPA_TYPE_Bytes;

	/* Copy data after the control structure */
	memcpy(buffer + *offset + sizeof(struct spa_pod_control), data, data_size);
	*offset += sizeof(struct spa_pod_control) + SPA_ROUND_UP_N(data_size, 8);

	return control;
}

static int test_ump_event_sort_offset_priority(void)
{
	uint8_t buffer[1024];
	size_t offset = 0;
	uint32_t ump_early = 0x20904060; /* Note On Ch 0 */
	uint32_t ump_late = 0x20904060;  /* Note On Ch 0 */

	struct spa_pod_control *a = create_mock_control(buffer, &offset, 100, SPA_CONTROL_UMP, &ump_early, 4);
	const void *abody = (uint8_t*)a + sizeof(struct spa_pod_control);

	struct spa_pod_control *b = create_mock_control(buffer, &offset, 200, SPA_CONTROL_UMP, &ump_late, 4);
	const void *bbody = (uint8_t*)b + sizeof(struct spa_pod_control);

	/* Earlier offset should sort before later offset */
	TEST_ASSERT(event_sort(a, abody, b, bbody) < 0, "Earlier offset should sort before later offset");
	/* Later offset should sort after earlier offset */
	TEST_ASSERT(event_sort(b, bbody, a, abody) > 0, "Later offset should sort after earlier offset");

	TEST_PASS();
}

static int test_ump_event_sort_same_offset_different_channels(void)
{
	uint8_t buffer[1024];
	size_t offset = 0;
	uint32_t ump_ch0 = 0x20904060; /* Note On Ch 0 */
	uint32_t ump_ch1 = 0x20914060; /* Note On Ch 1 */

	struct spa_pod_control *a = create_mock_control(buffer, &offset, 100, SPA_CONTROL_UMP, &ump_ch0, 4);
	const void *abody = (uint8_t*)a + sizeof(struct spa_pod_control);

	struct spa_pod_control *b = create_mock_control(buffer, &offset, 100, SPA_CONTROL_UMP, &ump_ch1, 4);
	const void *bbody = (uint8_t*)b + sizeof(struct spa_pod_control);

	/* Different channels at same offset should return 0 (no preference) */
	TEST_ASSERT(event_sort(a, abody, b, bbody) == 0, "Different channels at same offset should return 0");
	TEST_ASSERT(event_sort(b, bbody, a, abody) == 0, "Different channels at same offset should return 0");

	TEST_PASS();
}

static int test_ump_event_sort_priority_controller_vs_note(void)
{
	uint8_t buffer[1024];
	size_t offset = 0;
	uint32_t ump_note_on = 0x20904060;  /* Note On Ch 0 (priority 4) */
	uint32_t ump_controller = 0x20B04060; /* Controller Ch 0 (priority 2) */

	struct spa_pod_control *note_on = create_mock_control(buffer, &offset, 100, SPA_CONTROL_UMP, &ump_note_on, 4);
	const void *note_body = (uint8_t*)note_on + sizeof(struct spa_pod_control);

	struct spa_pod_control *controller = create_mock_control(buffer, &offset, 100, SPA_CONTROL_UMP, &ump_controller, 4);
	const void *ctrl_body = (uint8_t*)controller + sizeof(struct spa_pod_control);

	/* Controller (higher priority) should sort before Note On (lower priority) */
	TEST_ASSERT(event_sort(note_on, note_body, controller, ctrl_body) > 0, "Controller should sort before Note On");
	TEST_ASSERT(event_sort(controller, ctrl_body, note_on, note_body) <= 0, "Controller should sort before Note On");

	TEST_PASS();
}

static int test_event_compare_priority_table(void)
{
	/* Test controller (0xB0) vs note on (0x90) on same channel */
	TEST_ASSERT(event_compare(0x90, 0xB0) > 0, "Controller has higher priority than Note On");
	TEST_ASSERT(event_compare(0xB0, 0x90) < 0, "Controller has higher priority than Note On");

	/* Test program change (0xC0) vs note off (0x80) on same channel */
	TEST_ASSERT(event_compare(0x80, 0xC0) > 0, "Program change has higher priority than Note Off");
	TEST_ASSERT(event_compare(0xC0, 0x80) < 0, "Program change has higher priority than Note Off");

	/* Test different channels should return 0 */
	TEST_ASSERT(event_compare(0x90, 0x91) == 0, "Different channels should return 0");

	TEST_PASS();
}

int main(void)
{
	int result = 0;

	printf("Running mixer UMP sort tests...\n");

	printf("test_ump_event_sort_offset_priority: ");
	result |= test_ump_event_sort_offset_priority();

	printf("test_ump_event_sort_same_offset_different_channels: ");
	result |= test_ump_event_sort_same_offset_different_channels();

	printf("test_ump_event_sort_priority_controller_vs_note: ");
	result |= test_ump_event_sort_priority_controller_vs_note();

	printf("test_event_compare_priority_table: ");
	result |= test_event_compare_priority_table();

	if (result == 0) {
		printf("All tests passed!\n");
	} else {
		printf("Some tests failed!\n");
	}

	return result;
}