/* Spa Bluez5 Telephony D-Bus service */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_TELEPHONY_H
#define SPA_BLUEZ5_TELEPHONY_H

#include "defs.h"

enum spa_bt_telephony_call_state {
	CALL_STATE_ACTIVE,
	CALL_STATE_HELD,
	CALL_STATE_DIALING,
	CALL_STATE_ALERTING,
	CALL_STATE_INCOMING,
	CALL_STATE_WAITING,
	CALL_STATE_DISCONNECTED,
};

struct spa_bt_telephony {

};

struct spa_bt_telephony_ag {
	struct spa_bt_telephony *telephony;
};

struct spa_bt_telephony_call {
	struct spa_bt_telephony_ag *ag;

	/* D-Bus properties */
	char *line_identification;
	char *incoming_line;
	char *name;
	bool multiparty;
	enum spa_bt_telephony_call_state state;
};

struct spa_bt_telephony_ag_events {
#define SPA_VERSION_BT_TELEPHONY_AG_EVENTS	0
	uint32_t version;

	int (*dial)(void *data, const char *number);
	int (*swap_calls)(void *data);
	int (*release_and_answer)(void *data);
	int (*release_and_swap)(void *data);
	int (*hold_and_answer)(void *data);
	int (*hangup_all)(void *data);
	int (*create_multiparty)(void *data);
	int (*send_tones)(void *data, const char *tones);
};

struct spa_bt_telephony_call_events {
#define SPA_VERSION_BT_TELEPHONY_CALL_EVENTS	0
	uint32_t version;

	int (*answer)(void *data);
	int (*hangup)(void *data);
};

struct spa_bt_telephony *telephony_new(struct spa_log *log, struct spa_dbus *dbus,
					const struct spa_dict *info);
void telephony_free(struct spa_bt_telephony *telephony);

/* create/destroy the ag object */
struct spa_bt_telephony_ag * telephony_ag_new(struct spa_bt_telephony *telephony);
void telephony_ag_destroy(struct spa_bt_telephony_ag *ag);

void telephony_ag_add_listener(struct spa_bt_telephony_ag *ag,
			       struct spa_hook *listener,
			       const struct spa_bt_telephony_ag_events *events,
			       void *data);

/* register/unregister AudioGateway object on the bus */
int telephony_ag_register(struct spa_bt_telephony_ag *ag);
void telephony_ag_unregister(struct spa_bt_telephony_ag *ag);

//TODO
//struct spa_bt_telephony_call *telephony_ag_find_call_by_????(struct spa_bt_telephony_ag *ag, ???);
//void telephony_ag_iterate_calls(struct spa_bt_telephony_ag *ag, callback, userdata);

/* create/destroy the call object */
struct spa_bt_telephony_call * telephony_call_new(struct spa_bt_telephony_ag *ag);
void telephony_call_destroy(struct spa_bt_telephony_call *call);

/* register/unregister Call object on the bus */
int telephony_call_register(struct spa_bt_telephony_call *call);
void telephony_call_unregister(struct spa_bt_telephony_call *call);

/* send message to notify about property changes */
void telephony_call_notify_updated_props(struct spa_bt_telephony_call *call);

void telephony_call_add_listener(struct spa_bt_telephony_call *call,
				 struct spa_hook *listener,
				 const struct spa_bt_telephony_call_events *events,
				 void *data);

#endif
