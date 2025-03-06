/* Spa Bluez5 Telephony D-Bus service */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_TELEPHONY_H
#define SPA_BLUEZ5_TELEPHONY_H

#include "defs.h"

enum spa_bt_telephony_error {
	BT_TELEPHONY_ERROR_NONE = 0,
	BT_TELEPHONY_ERROR_FAILED,
	BT_TELEPHONY_ERROR_NOT_SUPPORTED,
	BT_TELEPHONY_ERROR_INVALID_FORMAT,
	BT_TELEPHONY_ERROR_INVALID_STATE,
	BT_TELEPHONY_ERROR_IN_PROGRESS,
	BT_TELEPHONY_ERROR_CME,
};

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

struct spa_bt_telephony_ag_transport {
	int8_t codec;
	enum spa_bt_transport_state state;
	dbus_bool_t rejectSCO;
};

struct spa_bt_telephony_ag {
	struct spa_bt_telephony *telephony;
	struct spa_list call_list;

	int id;

	/* D-Bus properties */
	char *address;
	int volume[SPA_BT_VOLUME_ID_TERM];
	struct spa_bt_telephony_ag_transport transport;
};

struct spa_bt_telephony_call {
	struct spa_bt_telephony_ag *ag;
	struct spa_list link;	/* link in ag->call_list */

	int id;

	/* D-Bus properties */
	char *line_identification;
	char *incoming_line;
	char *name;
	bool multiparty;
	enum spa_bt_telephony_call_state state;
};

struct spa_bt_telephony_ag_callbacks {
#define SPA_VERSION_BT_TELEPHONY_AG_CALLBACKS	0
	uint32_t version;

	void (*dial)(void *data, const char *number, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*swap_calls)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*release_and_answer)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*release_and_swap)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*hold_and_answer)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*hangup_all)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*create_multiparty)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*send_tones)(void *data, const char *tones, enum spa_bt_telephony_error *err, uint8_t *cme_error);

	void (*transport_activate)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);

	void (*set_speaker_volume)(void *data, uint8_t volume, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*set_microphone_volume)(void *data, uint8_t volume, enum spa_bt_telephony_error *err, uint8_t *cme_error);
};

struct spa_bt_telephony_call_callbacks {
#define SPA_VERSION_BT_TELEPHONY_CALL_CALLBACKS	0
	uint32_t version;

	void (*answer)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
	void (*hangup)(void *data, enum spa_bt_telephony_error *err, uint8_t *cme_error);
};

struct spa_bt_telephony *telephony_new(struct spa_log *log, struct spa_dbus *dbus,
					const struct spa_dict *info);
void telephony_free(struct spa_bt_telephony *telephony);


/* create/destroy the ag object */
struct spa_bt_telephony_ag * telephony_ag_new(struct spa_bt_telephony *telephony,
					      size_t user_data_size);
void telephony_ag_destroy(struct spa_bt_telephony_ag *ag);

/* get the user data structure; struct size is set when creating the AG */
void *telephony_ag_get_user_data(struct spa_bt_telephony_ag *ag);

void telephony_ag_set_callbacks(struct spa_bt_telephony_ag *ag,
			       const struct spa_bt_telephony_ag_callbacks *cbs,
			       void *data);

void telephony_ag_notify_updated_props(struct spa_bt_telephony_ag *ag);
void telephony_ag_transport_notify_updated_props(struct spa_bt_telephony_ag *ag);

/* register/unregister AudioGateway object on the bus */
int telephony_ag_register(struct spa_bt_telephony_ag *ag);
void telephony_ag_unregister(struct spa_bt_telephony_ag *ag);


/* create/destroy the call object */
struct spa_bt_telephony_call * telephony_call_new(struct spa_bt_telephony_ag *ag,
						  size_t user_data_size);
void telephony_call_destroy(struct spa_bt_telephony_call *call);

/* get the user data structure; struct size is set when creating the Call */
void *telephony_call_get_user_data(struct spa_bt_telephony_call *call);

/* register/unregister Call object on the bus */
int telephony_call_register(struct spa_bt_telephony_call *call);
void telephony_call_unregister(struct spa_bt_telephony_call *call);

/* send message to notify about property changes */
void telephony_call_notify_updated_props(struct spa_bt_telephony_call *call);

void telephony_call_set_callbacks(struct spa_bt_telephony_call *call,
				 const struct spa_bt_telephony_call_callbacks *cbs,
				 void *data);

#endif
