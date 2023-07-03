/* Spa Bluez5 ModemManager proxy */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_MODEMMANAGER_H_
#define SPA_BLUEZ5_MODEMMANAGER_H_

#include <spa/utils/list.h>

#include "defs.h"

enum cmee_error {
	CMEE_AG_FAILURE = 0,
	CMEE_NO_CONNECTION_TO_PHONE = 1,
	CMEE_OPERATION_NOT_ALLOWED = 3,
	CMEE_OPERATION_NOT_SUPPORTED = 4,
	CMEE_INVALID_CHARACTERS_TEXT_STRING = 25,
	CMEE_INVALID_CHARACTERS_DIAL_STRING = 27,
	CMEE_NO_NETWORK_SERVICE = 30
};

enum call_setup {
	CIND_CALLSETUP_NONE = 0,
	CIND_CALLSETUP_INCOMING,
	CIND_CALLSETUP_DIALING,
	CIND_CALLSETUP_ALERTING
};

enum call_direction {
	CALL_OUTGOING,
	CALL_INCOMING
};

enum call_state {
	CLCC_ACTIVE,
	CLCC_HELD,
	CLCC_DIALING,
	CLCC_ALERTING,
	CLCC_INCOMING,
	CLCC_WAITING,
	CLCC_RESPONSE_AND_HOLD
};

struct call {
	struct spa_list link;
	unsigned int index;
	struct impl *this;
	DBusPendingCall *pending;

	char *path;
	char *number;
	bool call_indicator;
	enum call_direction direction;
	enum call_state state;
	bool multiparty;
};

struct mm_ops {
	void (*send_cmd_result)(bool success, enum cmee_error error, void *user_data);
	void (*set_modem_service)(bool available, void *user_data);
	void (*set_modem_signal_strength)(unsigned int strength, void *user_data);
	void (*set_modem_operator_name)(const char *name, void *user_data);
	void (*set_modem_own_number)(const char *number, void *user_data);
	void (*set_modem_roaming)(bool is_roaming, void *user_data);
	void (*set_call_active)(bool active, void *user_data);
	void (*set_call_setup)(enum call_setup value, void *user_data);
};

#ifdef HAVE_BLUEZ_5_BACKEND_NATIVE_MM
void *mm_register(struct spa_log *log, void *dbus_connection, const struct spa_dict *info,
                  const struct mm_ops *ops, void *user_data);
void mm_unregister(void *data);
bool mm_is_available(void *modemmanager);
unsigned int mm_supported_features(void);
bool mm_answer_call(void *modemmanager, void *user_data, enum cmee_error *error);
bool mm_hangup_call(void *modemmanager, void *user_data, enum cmee_error *error);
bool mm_do_call(void *modemmanager, const char* number, void *user_data, enum cmee_error *error);
bool mm_send_dtmf(void *modemmanager, const char *dtmf, void *user_data, enum cmee_error *error);
const char *mm_get_incoming_call_number(void *modemmanager);
struct spa_list *mm_get_calls(void *modemmanager);
#else
static inline void *mm_register(struct spa_log *log, void *dbus_connection, const struct spa_dict *info,
                  const struct mm_ops *ops, void *user_data)
{
	return NULL;
}

static inline void mm_unregister(void *data)
{
}

static inline bool mm_is_available(void *modemmanager)
{
	return false;
}

static inline unsigned int mm_supported_features(void)
{
	return 0;
}

static inline bool mm_answer_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

static inline bool mm_hangup_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

static inline bool mm_do_call(void *modemmanager, const char* number, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

static inline bool mm_send_dtmf(void *modemmanager, const char *dtmf, void *user_data, enum cmee_error *error)
{
	if (error)
		*error = CMEE_OPERATION_NOT_SUPPORTED;
	return false;
}

static inline const char *mm_get_incoming_call_number(void *modemmanager)
{
	return NULL;
}

static inline struct spa_list *mm_get_calls(void *modemmanager)
{
	return NULL;
}
#endif

#endif
