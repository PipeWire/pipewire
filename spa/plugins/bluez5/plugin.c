/* Spa Volume plugin */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>

#include <spa/support/plugin.h>

extern const struct spa_handle_factory spa_bluez5_dbus_factory;
extern const struct spa_handle_factory spa_bluez5_device_factory;
extern const struct spa_handle_factory spa_media_sink_factory;
extern const struct spa_handle_factory spa_media_source_factory;
extern const struct spa_handle_factory spa_sco_sink_factory;
extern const struct spa_handle_factory spa_sco_source_factory;
extern const struct spa_handle_factory spa_a2dp_sink_factory;
extern const struct spa_handle_factory spa_a2dp_source_factory;
extern const struct spa_handle_factory spa_bluez5_midi_enum_factory;
extern const struct spa_handle_factory spa_bluez5_midi_node_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_bluez5_dbus_factory;
		break;
	case 1:
		*factory = &spa_bluez5_device_factory;
		break;
	case 2:
		*factory = &spa_media_sink_factory;
		break;
	case 3:
		*factory = &spa_media_source_factory;
		break;
	case 4:
		*factory = &spa_sco_sink_factory;
		break;
	case 5:
		*factory = &spa_sco_source_factory;
		break;
	case 6:
		*factory = &spa_a2dp_sink_factory;
		break;
	case 7:
		*factory = &spa_a2dp_source_factory;
		break;
	case 8:
		*factory = &spa_bluez5_midi_enum_factory;
		break;
	case 9:
		*factory = &spa_bluez5_midi_node_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
