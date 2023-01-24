#include <spa/utils/defs.h>

#include "midi.h"

#define TIME_HI(v) (0x80 | ((v >> 7) & 0x3f))
#define TIME_LO(v) (0x80 | (v & 0x7f))

struct event {
	uint16_t time_msec;
	size_t size;
	const uint8_t *data;
};

struct packet {
	size_t size;
	const uint8_t *data;
};

struct test_info {
	const struct packet *packets;
	const struct event *events;
	unsigned int i;
};

static const struct packet midi_1_packets[] = {
	{
		.size = 27,
		.data = (uint8_t[]) {
			TIME_HI(0x1234),
			/* event 1 */
			TIME_LO(0x1234), 0xa0, 0x01, 0x02,
			/* event 2: running status */
			0x03, 0x04,
			/* event 3: running status with timestamp */
			TIME_LO(0x1235), 0x05, 0x06,
			/* event 4 */
			TIME_LO(0x1236), 0xf8,
			/* event 5: sysex */
			TIME_LO(0x1237), 0xf0, 0x0a, 0x0b, 0x0c,
			/* event 6: realtime event inside sysex */
			TIME_LO(0x1238), 0xff,
			/* event 5 continues */
			0x0d, 0x0e, TIME_LO(0x1239), 0xf7,
			/* event 6: sysex */
			TIME_LO(0x1240), 0xf0, 0x10, 0x11,
			/* packet end in middle of sysex */
		},
	},
	{
		.size = 7,
		.data = (uint8_t[]) {
			TIME_HI(0x1241),
			/* event 6: continued from previous packet */
			0x12, TIME_LO(0x1241), 0xf7,
			/* event 7 */
			TIME_LO(0x1242), 0xf1, 0x13,
		}
	},
	{0}
};

static const struct event midi_1_events[] = {
	{ 0x1234, 3, (uint8_t[]) { 0xa0, 0x01, 0x02 } },
	{ 0x1234, 3, (uint8_t[]) { 0xa0, 0x03, 0x04 } },
	{ 0x1235, 3, (uint8_t[]) { 0xa0, 0x05, 0x06 } },
	{ 0x1236, 1, (uint8_t[]) { 0xf8 } },
	/* realtime event inside sysex come before it */
	{ 0x1238, 1, (uint8_t[]) { 0xff } },
	/* sysex timestamp indicates the end time; sysex contains the end marker */
	{ 0x1239, 7, (uint8_t[]) { 0xf0, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0xf7 } },
	{ 0x1241, 5, (uint8_t[]) { 0xf0, 0x10, 0x11, 0x12, 0xf7 } },
	{ 0x1242, 2, (uint8_t[]) { 0xf1, 0x13 } },
	{0}
};

static const struct packet midi_1_packets_mtu14[] = {
	{
		.size = 11,
		.data = (uint8_t[]) {
			TIME_HI(0x1234),
			TIME_LO(0x1234), 0xa0, 0x01, 0x02,
			0x03, 0x04,
			/* output Apple-style BLE; running status only for coincident time */
			TIME_LO(0x1235), 0xa0, 0x05, 0x06,
		},
	},
	{
		.size = 11,
		.data = (uint8_t[]) {
			TIME_HI(0x1236),
			TIME_LO(0x1236), 0xf8,
			TIME_LO(0x1238), 0xff,
			TIME_LO(0x1239), 0xf0, 0x0a, 0x0b, 0x0c, 0x0d,
		},
	},
	{
		.size = 11,
		.data = (uint8_t[]) {
			TIME_HI(0x1239),
			0x0e, TIME_LO(0x1239), 0xf7,
			TIME_LO(0x1241), 0xf0, 0x10, 0x11, 0x12, TIME_LO(0x1241), 0xf7
		},
	},
	{
		.size = 4,
		.data = (uint8_t[]) {
			TIME_HI(0x1242),
			TIME_LO(0x1242), 0xf1, 0x13
		},
	},
	{0}
};

static const struct packet midi_2_packets[] = {
	{
		.size = 9,
		.data = (uint8_t[]) {
			TIME_HI(0x1234),
			/* event 1 */
			TIME_LO(0x1234), 0xa0, 0x01, 0x02,
			/* event 2: timestamp low bits rollover */
			TIME_LO(0x12b3), 0xa0, 0x03, 0x04,
		},
	},
	{
		.size = 5,
		.data = (uint8_t[]) {
			TIME_HI(0x18b3),
			/* event 3: timestamp high bits jump */
			TIME_LO(0x18b3), 0xa0, 0x05, 0x06,
		},
	},
	{0}
};

static const struct event midi_2_events[] = {
	{ 0x1234, 3, (uint8_t[]) { 0xa0, 0x01, 0x02 } },
	{ 0x12b3, 3, (uint8_t[]) { 0xa0, 0x03, 0x04 } },
	{ 0x18b3, 3, (uint8_t[]) { 0xa0, 0x05, 0x06 } },
	{0}
};

static const struct packet midi_2_packets_mtu11[] = {
	/* Small MTU: only room for one event per packet */
	{
		.size = 5,
		.data = (uint8_t[]) {
			TIME_HI(0x1234), TIME_LO(0x1234), 0xa0, 0x01, 0x02,
		},
	},
	{
		.size = 5,
		.data = (uint8_t[]) {
			TIME_HI(0x12b3), TIME_LO(0x12b3), 0xa0, 0x03, 0x04,
		},
	},
	{
		.size = 5,
		.data = (uint8_t[]) {
			TIME_HI(0x18b3), TIME_LO(0x18b3), 0xa0, 0x05, 0x06,
		},
	},
	{0}
};


static void check_event(void *user_data, uint16_t time, uint8_t *event, size_t event_size)
{
	struct test_info *info = user_data;
	const struct event *ev = &info->events[info->i];

	spa_assert_se(ev->size > 0);
	spa_assert_se(ev->time_msec == time);
	spa_assert_se(ev->size == event_size);
	spa_assert_se(memcmp(event, ev->data, ev->size) == 0);

	++info->i;
}

static void check_parser(struct test_info *info)
{
	struct spa_bt_midi_parser parser;
	int res;
	int i;

	info->i = 0;

	spa_bt_midi_parser_init(&parser);
	for (i = 0; info->packets[i].size > 0; ++i) {
		res = spa_bt_midi_parser_parse(&parser,
				info->packets[i].data, info->packets[i].size,
				false, check_event, info);
		spa_assert_se(res == 0);
	}
	spa_assert_se(info->events[info->i].size == 0);
}

static void check_writer(struct test_info *info, unsigned int mtu)
{
	struct spa_bt_midi_writer writer;
	struct spa_bt_midi_parser parser;
	unsigned int i, packet;
	void SPA_UNUSED *buf = writer.buf;

	spa_bt_midi_parser_init(&parser);
	spa_bt_midi_writer_init(&writer, mtu);

	packet = 0;
	info->i = 0;

	for (i = 0; info->events[i].size > 0; ++i) {
		const struct event *ev = &info->events[i];
		bool last = (info->events[i+1].size == 0);
		int res;

		do {
			res = spa_bt_midi_writer_write(&writer,
					ev->time_msec * SPA_NSEC_PER_MSEC, ev->data, ev->size);
			spa_assert_se(res >= 0);
			if (res || last) {
				int r;

				spa_assert_se(info->packets[packet].size > 0);
				spa_assert_se(writer.size == info->packets[packet].size);
				spa_assert_se(memcmp(writer.buf, info->packets[packet].data, writer.size) == 0);
				++packet;

				/* Test roundtrip */
				r = spa_bt_midi_parser_parse(&parser, writer.buf, writer.size,
						false, check_event, info);
				spa_assert_se(r == 0);
			}
		} while (res);
	}

	spa_assert_se(info->packets[packet].size == 0);
	spa_assert_se(info->events[info->i].size == 0);
}

static void test_midi_parser_1(void)
{
	struct test_info info = {
		.packets = midi_1_packets,
		.events = midi_1_events,
	};

	check_parser(&info);
}

static void test_midi_parser_2(void)
{
	struct test_info info = {
		.packets = midi_2_packets,
		.events = midi_2_events,
	};

	check_parser(&info);
}

static void test_midi_writer_1(void)
{
	struct test_info info = {
		.packets = midi_1_packets_mtu14,
		.events = midi_1_events,
	};

	check_writer(&info, 14);
}

static void test_midi_writer_2(void)
{
	struct test_info info = {
		.packets = midi_2_packets,
		.events = midi_2_events,
	};

	check_writer(&info, 23);
	check_writer(&info, 12);
}

static void test_midi_writer_3(void)
{
	struct test_info info = {
		.packets = midi_2_packets_mtu11,
		.events = midi_2_events,
	};

	check_writer(&info, 11);
}

int main(void)
{
	test_midi_parser_1();
	test_midi_parser_2();
	test_midi_writer_1();
	test_midi_writer_2();
	test_midi_writer_3();
	return 0;
}
