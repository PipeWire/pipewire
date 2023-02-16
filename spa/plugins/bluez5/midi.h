/* Spa V4l2 dbus */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BT_MIDI_H_
#define SPA_BT_MIDI_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <spa/utils/defs.h>
#include <spa/support/log.h>

#define BLUEZ_SERVICE			"org.bluez"
#define BLUEZ_ADAPTER_INTERFACE		BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_DEVICE_INTERFACE		BLUEZ_SERVICE ".Device1"
#define BLUEZ_GATT_MANAGER_INTERFACE	BLUEZ_SERVICE ".GattManager1"
#define BLUEZ_GATT_PROFILE_INTERFACE	BLUEZ_SERVICE ".GattProfile1"
#define BLUEZ_GATT_SERVICE_INTERFACE	BLUEZ_SERVICE ".GattService1"
#define BLUEZ_GATT_CHR_INTERFACE	BLUEZ_SERVICE ".GattCharacteristic1"
#define BLUEZ_GATT_DSC_INTERFACE	BLUEZ_SERVICE ".GattDescriptor1"

#define BT_MIDI_SERVICE_UUID		"03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define BT_MIDI_CHR_UUID		"7772e5db-3868-4112-a1a9-f2669d106bf3"
#define BT_GATT_CHARACTERISTIC_USER_DESCRIPTION_UUID	"00002901-0000-1000-8000-00805f9b34fb"

#define MIDI_BUF_SIZE		8192
#define MIDI_MAX_MTU		8192

#define MIDI_CLOCK_PERIOD_MSEC	0x2000
#define MIDI_CLOCK_PERIOD_NSEC	(MIDI_CLOCK_PERIOD_MSEC * SPA_NSEC_PER_MSEC)

struct spa_bt_midi_server
{
	const char *chr_path;
};

struct spa_bt_midi_parser {
	unsigned int size;
	unsigned int sysex:1;
	uint8_t buf[MIDI_BUF_SIZE];
};

struct spa_bt_midi_writer {
	unsigned int size;
	unsigned int mtu;
	unsigned int pos;
	uint8_t running_status;
	uint64_t running_time_msec;
	unsigned int flush:1;
	uint8_t buf[MIDI_MAX_MTU];
};

struct spa_bt_midi_server_cb
{
	int (*acquire_notify)(void *user_data, int fd, uint16_t mtu);
	int (*acquire_write)(void *user_data, int fd, uint16_t mtu);
	int (*release)(void *user_data);
	const char *(*get_description)(void *user_data);
};

static inline void spa_bt_midi_parser_init(struct spa_bt_midi_parser *parser)
{
	parser->size = 0;
	parser->sysex = 0;
}

static inline void spa_bt_midi_parser_dup(struct spa_bt_midi_parser *src, struct spa_bt_midi_parser *dst, bool only_time)
{
	dst->size = src->size;
	dst->sysex = src->sysex;
	if (!only_time)
		memcpy(dst->buf, src->buf, src->size);
}

/**
 * Parse a single BLE MIDI data packet to normalized MIDI events.
 */
int spa_bt_midi_parser_parse(struct spa_bt_midi_parser *parser,
		const uint8_t *src, size_t src_size,
		bool only_time,
		void (*event)(void *user_data, uint16_t time, uint8_t *event, size_t event_size),
		void *user_data);

static inline void spa_bt_midi_writer_init(struct spa_bt_midi_writer *writer, unsigned int mtu)
{
	writer->size = 0;
	writer->mtu = SPA_MIN(mtu, (unsigned int)MIDI_MAX_MTU);
	writer->pos = 0;
	writer->running_status = 0;
	writer->running_time_msec = 0;
	writer->flush = 0;
}

/**
 * Add a new event to midi writer buffer.
 *
 * spa_bt_midi_writer_init(&writer, mtu);
 * for (time, event, size) in midi events {
 *     do {
 *         res = spa_bt_midi_writer_write(&writer, time, event, size);
 *         if (res < 0) {
 *             fail with error
 *         } else if (res) {
 *             send_packet(writer->buf, writer->size);
 *         }
 *     } while (res);
 * }
 * if (writer.size > 0)
 *     send_packet(writer->buf, writer->size);
 */
int spa_bt_midi_writer_write(struct spa_bt_midi_writer *writer,
		uint64_t time, const uint8_t *event, size_t event_size);

struct spa_bt_midi_server *spa_bt_midi_server_new(const struct spa_bt_midi_server_cb *cb,
		GDBusConnection *conn, struct spa_log *log, void *user_data);
void spa_bt_midi_server_released(struct spa_bt_midi_server *server, bool write);
void spa_bt_midi_server_destroy(struct spa_bt_midi_server *server);

#endif
