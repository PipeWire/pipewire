/* Spa Bluez5 AVRCP Player
 *
 * Copyright © 2021 Pauli Virtanen
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
