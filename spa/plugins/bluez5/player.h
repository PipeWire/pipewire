/* Spa Bluez5 AVRCP Player */
/* SPDX-FileCopyrightText: Copyright © 2021 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_PLAYER_H_
#define SPA_BLUEZ5_PLAYER_H_

enum spa_bt_player_state {
	SPA_BT_PLAYER_STOPPED,
	SPA_BT_PLAYER_PLAYING,
};

/**
 * Dummy AVRCP player.
 *
 * Some headsets require an AVRCP player to be present, before their
 * AVRCP volume synchronization works. To work around this, we
 * register a dummy player that does nothing.
 */
struct spa_bt_player {
	enum spa_bt_player_state state;
};

struct spa_bt_player *spa_bt_player_new(void *dbus_connection, struct spa_log *log);
void spa_bt_player_destroy(struct spa_bt_player *player);
int spa_bt_player_set_state(struct spa_bt_player *player,
		enum spa_bt_player_state state);
int spa_bt_player_register(struct spa_bt_player *player, const char *adapter_path);
int spa_bt_player_unregister(struct spa_bt_player *player, const char *adapter_path);

#endif
