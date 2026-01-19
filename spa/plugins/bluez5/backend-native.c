/* Spa HSP/HFP native backend */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021 Collabora */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include <dbus/dbus.h>

#include <spa/debug/mem.h>
#include <spa/debug/log.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/utils/json.h>
#include <spa/param/audio/raw.h>
#include <spa-private/dbus-helpers.h>

#include "defs.h"
#include "media-codecs.h"
#include "hfp-codec-caps.h"

#ifdef HAVE_LIBUSB
#include <libusb.h>
#endif

#include "modemmanager.h"
#include "upower.h"
#include "telephony.h"

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.bluez5.native");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define PROP_KEY_ROLES "bluez5.roles"
#define PROP_KEY_HEADSET_ROLES "bluez5.headset-roles"
#define PROP_KEY_HFP_DISABLE_NREC "bluez5.hfp-hf.disable-nrec"
#define PROP_KEY_HFP_DEFAULT_MIC_VOL "bluez5.hfp-hf.default-mic-volume"
#define PROP_KEY_HFP_DEFAULT_SPEAKER_VOL "bluez5.hfp-hf.default-speaker-volume"

#define HFP_CODEC_SWITCH_INITIAL_TIMEOUT_MSEC 5000
#define HFP_CODEC_SWITCH_TIMEOUT_MSEC 20000

#define INTERNATIONAL_NUMBER 145
#define NATIONAL_NUMBER 129

#define MAX_HF_INDICATORS 16

#define RFCOMM_MESSAGE_MAX_LENGTH 256

enum {
	HFP_AG_INITIAL_CODEC_SETUP_NONE = 0,
	HFP_AG_INITIAL_CODEC_SETUP_SEND,
	HFP_AG_INITIAL_CODEC_SETUP_WAIT
};

#define CIND_INDICATORS "(\"service\",(0-1)),(\"call\",(0-1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0-1)),(\"battchg\",(0-5))"
enum {
	CIND_SERVICE = 1,
	CIND_CALL,
	CIND_CALLSETUP,
	CIND_CALLHELD,
	CIND_SIGNAL,
	CIND_ROAM,
	CIND_BATTERY_LEVEL,
	CIND_MAX
};

struct modem {
	bool network_has_service;
	unsigned int signal_strength;
	bool network_is_roaming;
	char *operator_name;
	char *own_number;
	bool active_call;
	unsigned int call_setup;
};

struct impl {
	struct spa_bt_backend this;

	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;
	struct spa_loop_utils *loop_utils;
	struct spa_dbus *dbus;
	DBusConnection *conn;

	const struct media_codec * const * codecs;

#define DEFAULT_ENABLED_PROFILES (SPA_BT_PROFILE_HFP_HF | SPA_BT_PROFILE_HFP_AG)
	enum spa_bt_profile enabled_profiles;
	bool hfp_disable_nrec;
	int hfp_default_mic_volume;
	int hfp_default_speaker_volume;

	struct spa_source sco;
	unsigned int hfphsp_sco_datapath;

	const struct spa_bt_quirks *quirks;

	struct spa_list rfcomm_list;
	unsigned int defer_setup_enabled:1;

	struct modem modem;
	unsigned int battery_level;

	void *modemmanager;
	struct spa_source *ring_timer;
	void *upower;
	struct spa_bt_telephony *telephony;
	bool pts;
};

struct transport_data {
	struct rfcomm *rfcomm;
	struct spa_source sco;
	int err;
	bool requesting;
};

enum hfp_hf_state {
	hfp_hf_idle,
	hfp_hf_brsf,
	hfp_hf_bac,
	hfp_hf_cind1,
	hfp_hf_cind2,
	hfp_hf_cmer,
	hfp_hf_chld,
	hfp_hf_clip,
	hfp_hf_ccwa,
	hfp_hf_cmee,
	hfp_hf_nrec,
	hfp_hf_clcc,
	hfp_hf_vgs,
	hfp_hf_clcc_update,
	hfp_hf_chld1_hangup
};

enum hsp_hs_state {
	hsp_hs_init1,
	hsp_hs_init2,
	hsp_hs_vgs,
	hsp_hs_vgm,
};

struct rfcomm_volume {
	bool active;
	int hw_volume;
};

struct rfcomm_call_data {
	struct rfcomm *rfcomm;
	struct spa_bt_telephony_call *call;
};

struct rfcomm_cmd {
	struct spa_list link;
	int next_state;
	DBusMessage *msg;
	char cmd[RFCOMM_MESSAGE_MAX_LENGTH + 1];
};

struct codec_item {
	struct spa_list link;
	const struct media_codec *codec;
};

struct updated_call {
	struct spa_list link;
	int id;
};

struct rfcomm {
	struct spa_list link;
	struct spa_source source;
	struct impl *backend;
	struct spa_bt_device *device;
	struct spa_hook device_listener;
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;
	enum spa_bt_profile profile;
	struct spa_source timer;
	struct spa_source *volume_sync_timer;
	char* path;
	bool has_volume;
	struct rfcomm_volume volumes[SPA_BT_VOLUME_ID_TERM];
	unsigned int broken_mic_hw_volume:1;

	unsigned int hfp_cmd_in_progress:1;
	union {
		enum hfp_hf_state hf_state;
		enum hsp_hs_state hs_state;
		int hf_or_hs_state;
	};
	struct spa_list cmd_send_queue;	// elements: struct rfcomm_cmd

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	struct spa_list available_codec_list;
	struct spa_list supported_codec_list;
	unsigned int slc_configured:1;
	unsigned int codec_negotiation_supported:1;
	unsigned int hfp_ag_switching_codec:1;
	unsigned int hfp_ag_initial_codec_setup:2;
	unsigned int cind_call_active:1;
	unsigned int cind_call_notify:1;
	unsigned int extended_error_reporting:1;
	unsigned int clip_notify:1;
	unsigned int hfp_hf_3way:1;
	unsigned int hfp_hf_nrec:1;
	unsigned int hfp_hf_clcc:1;
	unsigned int hfp_hf_cme:1;
	unsigned int hfp_hf_in_progress:1;
	unsigned int chld_supported:1;
	unsigned int codec;
	uint32_t cind_enabled_indicators;
	char *hf_indicators[MAX_HF_INDICATORS];
	struct spa_bt_telephony_ag *telephony_ag;
	struct spa_list updated_call_list;
	char *dialing_number;
#endif
};

static const struct media_codec *codec_list_get(struct impl *backend, struct spa_list *list, unsigned int codec_id)
{
	struct codec_item *item;

	/* CVSD is always supported: not included in the list */
	if (codec_id == HFP_AUDIO_CODEC_CVSD)
		return spa_bt_get_hfp_codec(backend->monitor, codec_id);

	spa_list_for_each(item, list, link)
		if (item->codec->codec_id == codec_id)
			return item->codec;

	return NULL;
}

static bool codec_list_add(struct spa_list *list, const struct media_codec *codec)
{
	struct codec_item *item;

	if (codec->codec_id == HFP_AUDIO_CODEC_CVSD)
		return true;

	spa_list_for_each(item, list, link)
		if (item->codec == codec)
			return true;

	item = calloc(1, sizeof(*item));
	if (!item)
		return false;

	item->codec = codec;
	spa_list_append(list, &item->link);
	return true;
}

static void codec_list_clear(struct spa_list *list)
{
	struct codec_item *item;

	spa_list_consume(item, list, link) {
		spa_list_remove(&item->link);
		free(item);
	}
}

static const struct media_codec *codec_list_best(struct impl *backend, struct spa_list *list)
{
	size_t i;

	/* Codec list is in 'best' order */
	for (i = 0; backend->codecs[i]; ++i) {
		const struct media_codec *c = backend->codecs[i];
		if (c->kind == MEDIA_CODEC_HFP && codec_list_get(backend, list, c->codec_id))
			return c;
	}

	spa_assert_not_reached();
	return NULL;
}

static void sco_offload_btcodec(struct impl *backend, int sock, bool msbc)
{
	int err;
	char buffer[255];
	struct bt_codecs *codecs;

	if (backend->hfphsp_sco_datapath == HFP_SCO_DEFAULT_DATAPATH)
		return;

	spa_log_info(backend->log, "sock(%d) msbc(%d)", sock, msbc);

	memset(buffer, 0, sizeof(buffer));
	codecs = (void *)buffer;
	if (msbc)
		codecs->codecs[0].id = 0x05;
	else
		codecs->codecs[0].id = 0x02;
	codecs->num_codecs = 1;
	codecs->codecs[0].data_path_id = backend->hfphsp_sco_datapath;
	codecs->codecs[0].num_caps = 0x00;

	err = setsockopt(sock, SOL_BLUETOOTH, BT_CODEC, codecs, sizeof(buffer));
	if (err < 0)
		spa_log_error(backend->log, "ERROR: %s (%d)", strerror(errno), errno);
	else
		spa_log_info(backend->log, "set offload codec succeeded");
}

static DBusHandlerResult profile_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	if (!reply_with_error(conn, m, BLUEZ_PROFILE_INTERFACE ".Error.NotImplemented", "Method not implemented"))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void transport_destroy(void *data)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;

	spa_log_debug(backend->log, "transport %p destroy", rfcomm->transport);
	rfcomm->transport = NULL;
}

static void transport_state_changed (void *data, enum spa_bt_transport_state old,
			enum spa_bt_transport_state state)
{
	struct rfcomm *rfcomm = data;
	if (rfcomm->telephony_ag) {
		rfcomm->telephony_ag->transport.state = state;
		telephony_ag_transport_notify_updated_props(rfcomm->telephony_ag);
	}
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
	.destroy = transport_destroy,
	.state_changed = transport_state_changed,
};

static const struct spa_bt_transport_implementation sco_transport_impl;

static int rfcomm_new_transport(struct rfcomm *rfcomm, int codec_id)
{
	struct impl *backend = rfcomm->backend;
	const struct media_codec *codec;
	struct spa_bt_transport *t = NULL;
	struct transport_data *td;
	char* pathfd;

	if (rfcomm->transport) {
		spa_hook_remove(&rfcomm->transport_listener);
		spa_bt_transport_free(rfcomm->transport);
		rfcomm->transport = NULL;
	}

	codec = spa_bt_get_hfp_codec(backend->monitor, codec_id);
	if (!codec) {
		spa_log_warn(backend->log, "failed to get HFP codec %d", codec_id);
		goto fail;
	}

	if ((pathfd = spa_aprintf("%s/fd%d", rfcomm->path, rfcomm->source.fd)) == NULL)
		goto fail;

	t = spa_bt_transport_create(backend->monitor, pathfd, sizeof(struct transport_data));
	if (t == NULL) {
		free(pathfd);
		goto fail;
	}
	spa_bt_transport_set_implementation(t, &sco_transport_impl, t);

	t->device = rfcomm->device;
	spa_list_append(&t->device->transport_list, &t->device_link);
	t->profile = rfcomm->profile;
	t->backend = &backend->this;
	t->n_channels = 1;
	t->channels[0] = SPA_AUDIO_CHANNEL_MONO;
	t->media_codec = codec;

	td = t->user_data;
	td->rfcomm = rfcomm;

	if (t->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) {
		t->volumes[SPA_BT_VOLUME_ID_RX].volume = DEFAULT_AG_VOLUME;
		t->volumes[SPA_BT_VOLUME_ID_TX].volume = DEFAULT_AG_VOLUME;
	} else {
		t->volumes[SPA_BT_VOLUME_ID_RX].volume = DEFAULT_RX_VOLUME;
		t->volumes[SPA_BT_VOLUME_ID_TX].volume = DEFAULT_TX_VOLUME;
	}

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM ; ++i) {
		t->volumes[i].active = rfcomm->volumes[i].active;
		t->volumes[i].hw_volume_max = SPA_BT_VOLUME_HS_MAX;
		if (rfcomm->volumes[i].active && rfcomm->volumes[i].hw_volume != SPA_BT_VOLUME_INVALID)
			t->volumes[i].volume = (float)
				spa_bt_volume_hw_to_linear(rfcomm->volumes[i].hw_volume, t->volumes[i].hw_volume_max);
	}

	spa_bt_transport_add_listener(t, &rfcomm->transport_listener, &transport_events, rfcomm);

	if (rfcomm->telephony_ag) {
		rfcomm->telephony_ag->transport.codec = codec_id;
		rfcomm->telephony_ag->transport.state = SPA_BT_TRANSPORT_STATE_IDLE;
		telephony_ag_transport_notify_updated_props(rfcomm->telephony_ag);
	}

	rfcomm->transport = t;
	return 0;

fail:
	spa_log_warn(backend->log, "failed to create transport");
	return -ENOMEM;
}

static int codec_switch_stop_timer(struct rfcomm *rfcomm);

static void volume_sync_stop_timer(struct rfcomm *rfcomm);

static void rfcomm_free(struct rfcomm *rfcomm)
{
	struct updated_call *updated_call;
	struct rfcomm_cmd *cmd;

	spa_list_consume(updated_call, &rfcomm->updated_call_list, link) {
		spa_list_remove(&updated_call->link);
		free(updated_call);
	}

	spa_list_consume(cmd, &rfcomm->cmd_send_queue, link) {
		if (cmd->msg) {
			telephony_send_dbus_method_reply(rfcomm->backend->telephony, cmd->msg, BT_TELEPHONY_ERROR_FAILED, 0);
			spa_clear_ptr(cmd->msg, dbus_message_unref);
		}

		spa_list_remove(&cmd->link);
		free(cmd);
	}

	codec_switch_stop_timer(rfcomm);
	if (rfcomm->telephony_ag) {
		telephony_ag_destroy(rfcomm->telephony_ag);
		rfcomm->telephony_ag = NULL;
	}
	for (int i = 0; i < MAX_HF_INDICATORS; i++) {
		if (rfcomm->hf_indicators[i]) {
			free(rfcomm->hf_indicators[i]);
		}
	}
	spa_list_remove(&rfcomm->link);
	if (rfcomm->path)
		free(rfcomm->path);
	if (rfcomm->transport) {
		spa_hook_remove(&rfcomm->transport_listener);
		spa_bt_transport_free(rfcomm->transport);
	}
	if (rfcomm->device) {
		spa_bt_device_report_battery_level(rfcomm->device, SPA_BT_NO_BATTERY);
		spa_hook_remove(&rfcomm->device_listener);
		rfcomm->device = NULL;
	}
	if (rfcomm->source.fd >= 0) {
		if (rfcomm->source.loop)
			spa_loop_remove_source(rfcomm->source.loop, &rfcomm->source);
		shutdown(rfcomm->source.fd, SHUT_RDWR);
		close (rfcomm->source.fd);
		rfcomm->source.fd = -1;
	}
	if (rfcomm->volume_sync_timer)
		spa_loop_utils_destroy_source(rfcomm->backend->loop_utils, rfcomm->volume_sync_timer);
	codec_list_clear(&rfcomm->available_codec_list);
	codec_list_clear(&rfcomm->supported_codec_list);
	free(rfcomm);
}

static void rfcomm_cmd_done(struct rfcomm *rfcomm, char *reply)
{
	struct impl *backend = rfcomm->backend;

	if (SPA_LIKELY (!spa_list_is_empty(&rfcomm->cmd_send_queue))) {
		struct rfcomm_cmd *cmd = NULL;
		cmd = spa_list_first(&rfcomm->cmd_send_queue, struct rfcomm_cmd, link);

		spa_log_debug(backend->log, "%s -> %s", cmd->cmd, reply);

		if (cmd->msg) {
			enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
			uint8_t cme_error = 0;

			if (spa_strstartswith(reply, "+CME ERROR:")) {
				cme_error = atoi(reply + strlen("+CME ERROR:"));
				err = BT_TELEPHONY_ERROR_CME;
			} else if (!spa_strstartswith(reply, "OK")) {
				err = BT_TELEPHONY_ERROR_FAILED;
			}

			telephony_send_dbus_method_reply(backend->telephony, cmd->msg, err, cme_error);
			spa_clear_ptr(cmd->msg, dbus_message_unref);
		}

		spa_list_remove(&cmd->link);
		free(cmd);
	} else {
		spa_log_warn(backend->log, "received response but no command was sent");
	}

	rfcomm->hfp_cmd_in_progress = false;
}

static ssize_t rfcomm_send_next_cmd(struct rfcomm *rfcomm)
{
	struct impl *backend = rfcomm->backend;
	struct rfcomm_cmd *cmd = NULL;
	char message[RFCOMM_MESSAGE_MAX_LENGTH + 1];
	ssize_t len = 0;

	if (rfcomm->hfp_cmd_in_progress)
		return -EINPROGRESS;

	if (spa_list_is_empty(&rfcomm->cmd_send_queue))
		return -ENODATA;

	cmd = spa_list_first(&rfcomm->cmd_send_queue, struct rfcomm_cmd, link);

	strncpy(message, cmd->cmd, RFCOMM_MESSAGE_MAX_LENGTH + 1);
	len = strlen(message);

	spa_log_debug(backend->log, "RFCOMM >> %s", message);

	/*
	 * The format of an AT command from the HF to the AG shall be: <AT command><cr>
	 * - HFP 1.8, 4.34.1
	 *
	 * The format for a command from the HS to the AG is thus: AT<cmd>=<value><cr>
	 * - HSP 1.2, 4.8.1
	 */
	message[len] = '\r';
	/* `message` is no longer null-terminated */

	len = write(rfcomm->source.fd, message, len + 1);
	/* we ignore any errors, it's not critical and real errors should
	 * be caught with the HANGUP and ERROR events handled above */
	if (len < 0) {
		len = -errno;
		spa_log_error(backend->log, "RFCOMM write error: %s", strerror(errno));
	}

	rfcomm->hfp_cmd_in_progress = true;
	rfcomm->hf_or_hs_state = cmd->next_state;

	return len;
}

/* from HF/HS to AG */
SPA_PRINTF_FUNC(4, 5)
static ssize_t rfcomm_send_cmd(struct rfcomm *rfcomm, int next_state, DBusMessage *m, const char *format, ...)
{
	struct impl *backend = rfcomm->backend;
	spa_autofree struct rfcomm_cmd *cmd = NULL;
	ssize_t len;
	va_list args;

	cmd = calloc(1, sizeof(struct rfcomm_cmd));

	va_start(args, format);
	len = vsnprintf(cmd->cmd, RFCOMM_MESSAGE_MAX_LENGTH + 1, format, args);
	va_end(args);

	if (len < 0)
		return -EINVAL;

	if (len > RFCOMM_MESSAGE_MAX_LENGTH)
		return -E2BIG;

	spa_log_debug(backend->log, "Queueing command: %s", cmd->cmd);

	cmd->next_state = next_state;
	cmd->msg = m ? dbus_message_ref(m) : NULL;
	spa_list_append(&rfcomm->cmd_send_queue, &cmd->link);
	cmd = NULL;

	return rfcomm_send_next_cmd(rfcomm);
}

/* from AG to HF/HS */
SPA_PRINTF_FUNC(2, 3)
static ssize_t rfcomm_send_reply(const struct rfcomm *rfcomm, const char *format, ...)
{
	struct impl *backend = rfcomm->backend;
	char message[RFCOMM_MESSAGE_MAX_LENGTH + 4];
	ssize_t len;
	va_list args;

	va_start(args, format);
	len = vsnprintf(&message[2], RFCOMM_MESSAGE_MAX_LENGTH + 1, format, args);
	va_end(args);

	if (len < 0)
		return -EINVAL;

	if (len > RFCOMM_MESSAGE_MAX_LENGTH)
		return -E2BIG;

	spa_log_debug(backend->log, "RFCOMM >> %s", &message[2]);

	/*
	 * The format of the OK code from the AG to the HF shall be: <cr><lf>OK<cr><lf>
	 * The format of the generic ERROR code from the AG to the HF shall be: <cr><lf>ERROR<cr><lf>
	 * The format of an unsolicited result code from the AG to the HF shall be: <cr><lf><result code><cr><lf>
	 * - HFP 1.8, 4.34.1
	 *
	 * If the command is processed successfully, the resulting response from the AG to the HS is: <cr><lf>OK<cr><lf>
	 * If the command is not processed successfully, or is not recognized,
	 * the resulting response from the AG to the HS is: <cr><lf>ERROR<cr><lf>
	 * The format for an unsolicited result code (such as RING) from the AG to the HS is: <cr><lf><result code><cr><lf>
	 * - HSP 1.2, 4.8.1
	 */
	message[0] = '\r';
	message[1] = '\n';
	message[len + 2] = '\r';
	message[len + 3] = '\n';
	/* `message` is no longer null-terminated */

	len = write(rfcomm->source.fd, message, len + 4);
	/* we ignore any errors, it's not critical and real errors should
	 * be caught with the HANGUP and ERROR events handled above */
	if (len < 0) {
		len = -errno;
		spa_log_error(backend->log, "RFCOMM write error: %s", strerror(errno));
	}

	return len;
}

static void rfcomm_send_error(const struct rfcomm *rfcomm, enum cmee_error error)
{
	if (rfcomm->extended_error_reporting)
		rfcomm_send_reply(rfcomm, "+CME ERROR: %d", error);
	else
		rfcomm_send_reply(rfcomm, "ERROR");
}

static bool rfcomm_hw_volume_enabled(struct rfcomm *rfcomm)
{
	return rfcomm->device != NULL
		&& (rfcomm->device->hw_volume_profiles & rfcomm->profile);
}

static void rfcomm_emit_volume_changed(struct rfcomm *rfcomm, int id, int hw_volume)
{
	struct spa_bt_transport_volume *t_volume;
	bool valid_volume = (id == SPA_BT_VOLUME_ID_RX || id == SPA_BT_VOLUME_ID_TX);

	if (valid_volume && hw_volume >= 0) {
		rfcomm->volumes[id].active = true;
		rfcomm->volumes[id].hw_volume = hw_volume;
	}

	spa_log_debug(rfcomm->backend->log, "volume changed %d", hw_volume);

	if (rfcomm_hw_volume_enabled(rfcomm)) {
		if (rfcomm->transport == NULL || !rfcomm->has_volume)
			return;

		for (int i = 0; i < SPA_BT_VOLUME_ID_TERM ; ++i) {
			t_volume = &rfcomm->transport->volumes[i];
			t_volume->active = rfcomm->volumes[i].active;
			t_volume->volume = (float)
				spa_bt_volume_hw_to_linear(rfcomm->volumes[i].hw_volume, t_volume->hw_volume_max);
		}

		spa_bt_transport_emit_volume_changed(rfcomm->transport);
	}

	if (rfcomm->telephony_ag && valid_volume) {
		rfcomm->telephony_ag->volume[id] = hw_volume;
		telephony_ag_notify_updated_props(rfcomm->telephony_ag);
	}
}

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
static bool rfcomm_hsp_ag(struct rfcomm *rfcomm, char* buf)
{
	struct impl *backend = rfcomm->backend;
	unsigned int gain;

	/* There are only three HSP AT commands:
	 * AT+VGS=value: value between 0 and 15, sent by the HS to AG to set the speaker gain.
	 * AT+VGM=value: value between 0 and 15, sent by the HS to AG to set the microphone gain.
	 * AT+CKPD=200: Sent by HS when headset button is pressed. */
	if (sscanf(buf, "AT+VGS=%d", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", buf);
			rfcomm_send_reply(rfcomm, "ERROR");
		}
	} else if (sscanf(buf, "AT+VGM=%d", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			if (!rfcomm->broken_mic_hw_volume)
				rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			rfcomm_send_reply(rfcomm, "ERROR");
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", buf);
		}
	} else if (spa_strstartswith(buf, "AT+CKPD=200") == 1) {
		rfcomm_send_reply(rfcomm, "OK");
		spa_bt_device_emit_switch_profile(rfcomm->device);
	} else {
		return false;
	}

	return true;
}

static void rfcomm_send_volume_cmd(struct rfcomm *rfcomm, int next_state, DBusMessage *m, int id)
{
	struct spa_bt_transport_volume *t_volume;
	const char *format;
	int hw_volume = rfcomm->volumes[id].hw_volume;

	if (rfcomm_hw_volume_enabled(rfcomm)) {
		t_volume = rfcomm->transport ? &rfcomm->transport->volumes[id] : NULL;

		if (t_volume && t_volume->active) {
			hw_volume = spa_bt_volume_linear_to_hw(t_volume->volume, t_volume->hw_volume_max);
			rfcomm->volumes[id].hw_volume = hw_volume;
		}
	}

	if (rfcomm->telephony_ag) {
		rfcomm->telephony_ag->volume[id] = hw_volume;
		telephony_ag_notify_updated_props(rfcomm->telephony_ag);
	}

	if (id == SPA_BT_VOLUME_ID_TX)
		format = "AT+VGM";
	else if (id == SPA_BT_VOLUME_ID_RX)
		format = "AT+VGS";
	else
	 	spa_assert_not_reached();

	rfcomm_send_cmd(rfcomm, next_state, m, "%s=%d", format, hw_volume);
}

static bool rfcomm_hsp_hs(struct rfcomm *rfcomm, char* buf)
{
	struct impl *backend = rfcomm->backend;
	unsigned int gain;

	/* There are only three HSP AT result codes:
	 * +VGS=value: value between 0 and 15, sent by AG to HS as a response to an AT+VGS command
	 *   or when the gain is changed on the AG side.
	 * +VGM=value: value between 0 and 15, sent by AG to HS as a response to an AT+VGM command
	 *   or when the gain is changed on the AG side.
	 * RING: Sent by AG to HS to notify of an incoming call. It can safely be ignored because
	 *   it does not expect a reply. */
	if (sscanf(buf, "+VGS=%d", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", buf);
		}
	} else if (sscanf(buf, "+VGM=%d", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", buf);
		}
	} else if (spa_streq(buf, "OK") || spa_streq(buf, "ERROR")) {
		rfcomm_cmd_done(rfcomm, buf);

		if (spa_streq(buf, "OK")) {
			if (rfcomm->hs_state == hsp_hs_init2) {
				rfcomm_send_volume_cmd(rfcomm, hsp_hs_vgs, NULL, SPA_BT_VOLUME_ID_RX);
			} else if (rfcomm->hs_state == hsp_hs_vgs) {
				rfcomm_send_volume_cmd(rfcomm, hsp_hs_vgm, NULL, SPA_BT_VOLUME_ID_TX);
			}
		}

		rfcomm_send_next_cmd(rfcomm);
	}

	return true;
}
#endif

#ifdef HAVE_LIBUSB
static bool check_usb_altsetting_6(struct impl *backend, uint16_t vendor_id, uint16_t product_id)
{
	libusb_context *ctx = NULL;
	struct libusb_config_descriptor *cfg = NULL;
	libusb_device **devices = NULL;

	ssize_t ndev, idev;
	int res;
	bool ok = false;

	if ((res = libusb_init(&ctx)) < 0) {
		ctx = NULL;
		goto fail;
	}

	if ((ndev = libusb_get_device_list(ctx, &devices)) < 0) {
		res = ndev;
		devices = NULL;
		goto fail;
	}

	for (idev = 0; idev < ndev; ++idev) {
		libusb_device *dev = devices[idev];
		struct libusb_device_descriptor desc;
		int icfg;

		libusb_get_device_descriptor(dev, &desc);
		if (vendor_id != desc.idVendor || product_id != desc.idProduct)
			continue;

		/* Check the device has Bluetooth isoch. altsetting 6 interface */

		for (icfg = 0; icfg < desc.bNumConfigurations; ++icfg) {
			int iiface;

			if ((res = libusb_get_config_descriptor(dev, icfg, &cfg)) != 0) {
				cfg = NULL;
				goto fail;
			}

			for (iiface = 0; iiface < cfg->bNumInterfaces; ++iiface) {
				const struct libusb_interface *iface = &cfg->interface[iiface];
				int ialt;

				for (ialt = 0; ialt < iface->num_altsetting; ++ialt) {
					const struct libusb_interface_descriptor *idesc = &iface->altsetting[ialt];
					int iep;

					if (idesc->bInterfaceClass != LIBUSB_CLASS_WIRELESS ||
							idesc->bInterfaceSubClass != 1 /* RF */ ||
							idesc->bInterfaceProtocol != 1 /* Bluetooth */ ||
							idesc->bAlternateSetting != 6)
						continue;

					for (iep = 0; iep < idesc->bNumEndpoints; ++iep) {
						const struct libusb_endpoint_descriptor *ep = &idesc->endpoint[iep];
						if ((ep->bmAttributes & 0x3) == 0x1 /* isochronous */) {
							ok = true;
							goto done;
						}
					}
				}
			}

			libusb_free_config_descriptor(cfg);
			cfg = NULL;
		}
	}

done:
	if (cfg)
		libusb_free_config_descriptor(cfg);
	if (devices)
		libusb_free_device_list(devices, true);
	if (ctx)
		libusb_exit(ctx);
	return ok;

fail:
	spa_log_info(backend->log, "failed to acquire USB device info: %d (%s)",
			res, libusb_strerror(res));
	ok = false;
	goto done;
}
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE

static bool device_supports_codec(struct impl *backend, struct spa_bt_device *device,
		enum spa_bluetooth_audio_codec codec)
{
	int res;
	bool alt6_ok = true, alt1_ok = true;
	bool msbc_alt6_ok = true, msbc_alt1_ok = true;
	uint32_t bt_features;

	if (device->adapter == NULL)
		return false;

	if (backend->quirks && spa_bt_quirks_get_features(backend->quirks, device->adapter, device, &bt_features) == 0) {
		msbc_alt1_ok = (bt_features & (SPA_BT_FEATURE_MSBC_ALT1 | SPA_BT_FEATURE_MSBC_ALT1_RTL));
		msbc_alt6_ok = (bt_features & SPA_BT_FEATURE_MSBC);
	}

	switch (codec) {
	case SPA_BLUETOOTH_AUDIO_CODEC_CVSD:
		return true;
	case SPA_BLUETOOTH_AUDIO_CODEC_MSBC:
		alt1_ok = msbc_alt1_ok;
		alt6_ok = msbc_alt6_ok;
		break;
	case SPA_BLUETOOTH_AUDIO_CODEC_LC3_SWB:
	default:
		/* LC3-SWB has same transport requirements as msbc.
		 * However, ALT1/ALT5 modes don't appear to work, seem
		 * to lose frame sync so output is garbled.
		 */
		alt1_ok = false;
		alt6_ok = msbc_alt6_ok;
		break;
	}

	spa_log_info(backend->log,
			"bluez-monitor/hardware.conf: alt6:%d alt1/5:%d", (int)alt6_ok, (int)alt1_ok);

	if (!alt6_ok && !alt1_ok)
		return false;

	res = spa_bt_adapter_has_msbc(device->adapter);
	if (res < 0) {
		spa_log_warn(backend->log,
				"adapter %s: failed to determine msbc/esco capability (%d)",
				device->adapter->path, res);
	} else if (res == 0) {
		spa_log_info(backend->log,
				"adapter %s: no msbc/esco transport",
				device->adapter->path);
		return false;
	} else {
		spa_log_debug(backend->log,
				"adapter %s: has msbc/esco transport",
				device->adapter->path);
	}

	/* Check if USB ALT6 is really available on the device */
	if (device->adapter->bus_type == BUS_TYPE_USB && !alt1_ok && alt6_ok) {
#ifdef HAVE_LIBUSB
		if (device->adapter->source_id == SOURCE_ID_USB) {
			alt6_ok = check_usb_altsetting_6(backend, device->adapter->vendor_id,
					device->adapter->product_id);
		} else {
			alt6_ok = false;
		}
		if (!alt6_ok)
			spa_log_info(backend->log, "bluetooth host adapter does not support USB ALT6");
#else
		spa_log_info(backend->log,
			"compiled without libusb; can't check if bluetooth adapter has USB ALT6, assuming no");
		alt6_ok = false;
#endif
	}
	if (device->adapter->bus_type != BUS_TYPE_USB)
		alt1_ok = false;

	return alt6_ok || alt1_ok;
}

static void make_available_codec_list(struct impl *backend, struct spa_bt_device *device, struct spa_list *codec_list)
{
	size_t i;

	codec_list_clear(codec_list);

	for (i = 0; backend->codecs[i]; ++i) {
		const struct media_codec *codec = backend->codecs[i];
		if (codec->kind != MEDIA_CODEC_HFP)
			continue;
		if (device_supports_codec(backend, device, codec->id))
			codec_list_add(codec_list, codec);
	}
}

static int codec_switch_start_timer(struct rfcomm *rfcomm, int timeout_msec);

static void process_xevent_indicator(struct rfcomm *rfcomm, unsigned int level, unsigned int nlevels)
{
	struct impl *backend = rfcomm->backend;
	uint8_t perc;

	spa_log_debug(backend->log, "AT+XEVENT level:%u nlevels:%u", level, nlevels);

	if (nlevels <= 1)
		return;

	/* 0 <= level < nlevels */
	perc = SPA_MIN(level, nlevels - 1) * 100 / (nlevels - 1);
	spa_bt_device_report_battery_level(rfcomm->device, perc);
}

static void process_iphoneaccev_indicator(struct rfcomm *rfcomm, unsigned int key, unsigned int value)
{
	struct impl *backend = rfcomm->backend;

	spa_log_debug(backend->log, "key:%u value:%u", key, value);

	switch (key) {
	case SPA_BT_HFP_HF_IPHONEACCEV_KEY_BATTERY_LEVEL: {
		// Battery level is reported in range of 0-9, convert to 10-100%
		uint8_t level = (SPA_CLAMP(value, 0u, 9u) + 1) * 10;
		spa_log_debug(backend->log, "battery level: %d%%", (int) level);

		// TODO: report without Battery Provider (using props)
		spa_bt_device_report_battery_level(rfcomm->device, level);
		break;
	}
	case SPA_BT_HFP_HF_IPHONEACCEV_KEY_DOCK_STATE:
		break;
	default:
		spa_log_warn(backend->log, "unknown AT+IPHONEACCEV key:%u value:%u", key, value);
		break;
	}
}

static void process_hfp_hf_indicator(struct rfcomm *rfcomm, unsigned int indicator, unsigned int value)
{
	struct impl *backend = rfcomm->backend;

	spa_log_debug(backend->log, "indicator:%u value:%u", indicator, value);

	switch (indicator) {
	case SPA_BT_HFP_HF_INDICATOR_ENHANCED_SAFETY:
		rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
		break;
	case SPA_BT_HFP_HF_INDICATOR_BATTERY_LEVEL:
		// Battery level is reported in range 0-100
		spa_log_debug(backend->log, "battery level: %u%%", value);

		if (value <= 100) {
			// TODO: report without Battery Provider (using props)
			spa_bt_device_report_battery_level(rfcomm->device, value);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_warn(backend->log, "battery HF indicator %u outside of range [0, 100]: %u", indicator, value);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
		}
		break;
	default:
		spa_log_warn(backend->log, "unknown HF indicator:%u value:%u", indicator, value);
		rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
		break;
	}
}

static void rfcomm_hfp_ag_set_cind(struct rfcomm *rfcomm, bool call_active)
{
	if (rfcomm->profile != SPA_BT_PROFILE_HFP_HF)
		return;

	if (call_active == rfcomm->cind_call_active)
		return;

	rfcomm->cind_call_active = call_active;

	if (!rfcomm->cind_call_notify)
		return;

	rfcomm_send_reply(rfcomm, "+CIEV: 2,%d", rfcomm->cind_call_active);
}

static bool rfcomm_hfp_ag(struct rfcomm *rfcomm, char* buf)
{
	struct impl *backend = rfcomm->backend;
	unsigned int features;
	unsigned int gain;
	unsigned int count, r;
	unsigned int selected_codec;
	unsigned int indicator;
	unsigned int indicator_value;
	unsigned int value;
	unsigned int xevent_level;
	unsigned int xevent_nlevels;
	int xapl_vendor;
	int xapl_product;
	int xapl_features;

	if (sscanf(buf, "AT+BRSF=%u", &features) == 1) {
		unsigned int ag_features = SPA_BT_HFP_AG_FEATURE_NONE;

		/*
		 * Determine device volume control. Some headsets only support control of
		 * TX volume, but not RX, even if they have a microphone. Determine this
		 * separately based on whether we also get AT+VGS/AT+VGM, and quirks.
		 */
		rfcomm->has_volume = (features & SPA_BT_HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL);

		/* Decide if we want to signal that the computer supports codec negotiation
		   This should be done when the computers bluetooth adapter supports the necessary transport mode */
		if (!spa_list_is_empty(&rfcomm->available_codec_list)) {
			/* set the feature bit that indicates AG (=computer) supports codec negotiation */
			ag_features |= SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION;

			/* let's see if the headset supports codec negotiation */
			if ((features & (SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION)) != 0) {
				spa_log_debug(backend->log,
					"RFCOMM features = %i, codec negotiation supported by headset",
					features);
				/* Prepare reply: Audio Gateway (=computer) supports codec negotiation */
				rfcomm->codec_negotiation_supported = true;
			} else {
				/* Codec negotiation not supported */
				spa_log_debug(backend->log,
					"RFCOMM features = %i, codec negotiation NOT supported by headset",
					 features);

				rfcomm->codec_negotiation_supported = false;
			}
		}

		/* send reply to HF with the features supported by Audio Gateway (=computer) */
		ag_features |= mm_supported_features();
		ag_features |= SPA_BT_HFP_AG_FEATURE_HF_INDICATORS | SPA_BT_HFP_AG_FEATURE_ESCO_S4;
		rfcomm_send_reply(rfcomm, "+BRSF: %u", ag_features);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+BAC=")) {
		/* retrieve supported codecs */
		/* response has the form AT+BAC=<codecID1>,<codecID2>,<codecIDx>
		   strategy: split the string into tokens */

		char* token;
		int cntr = 0;

		codec_list_clear(&rfcomm->supported_codec_list);

		while ((token = strsep(&buf, "=,"))) {
			unsigned int codec_id;

			/* skip token 0 i.e. the "AT+BAC=" part */
			if (cntr > 0 && sscanf(token, "%u", &codec_id) == 1) {
				const struct media_codec *codec;

				spa_log_debug(backend->log, "RFCOMM AT+BAC found codec %u", codec_id);

				codec = codec_list_get(backend, &rfcomm->available_codec_list, codec_id);
				if (codec) {
					spa_log_debug(backend->log, "RFCOMM AT+BAC codec %s supported", codec->description);
					codec_list_add(&rfcomm->supported_codec_list, codec);
				} else {
					spa_log_debug(backend->log, "RFCOMM AT+BAC codec %u not supported", codec_id);
				}
			}
			cntr++;
		}

		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CIND=?")) {
		rfcomm_send_reply(rfcomm, "+CIND:%s", CIND_INDICATORS);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CIND?")) {
		rfcomm_send_reply(rfcomm, "+CIND: %d,%d,%d,0,%d,%d,%d", backend->modem.network_has_service,
		                  backend->modem.active_call, backend->modem.call_setup, backend->modem.signal_strength,
		                  backend->modem.network_is_roaming, backend->battery_level);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CMER")) {
		const struct media_codec *best_codec = codec_list_best(backend, &rfcomm->supported_codec_list);
		int mode, keyp, disp, ind;

		rfcomm->slc_configured = true;
		rfcomm_send_reply(rfcomm, "OK");

		rfcomm->cind_call_active = false;
		if (sscanf(buf, "AT+CMER= %d , %d , %d , %d", &mode, &keyp, &disp, &ind) == 4)
			rfcomm->cind_call_notify = ind ? true : false;
		else
			rfcomm->cind_call_notify = false;

		/* switch to better codec by sending unsolicited +BCS message */
		if (rfcomm->codec_negotiation_supported && best_codec &&
				best_codec->id != SPA_BLUETOOTH_AUDIO_CODEC_CVSD) {
			spa_log_debug(backend->log, "RFCOMM initial codec setup");
			rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_SEND;
			rfcomm_send_reply(rfcomm, "+BCS: %u", best_codec->codec_id);
			codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_INITIAL_TIMEOUT_MSEC);
		} else {
			if (rfcomm_new_transport(rfcomm, HFP_AUDIO_CODEC_CVSD) < 0) {
				// TODO: We should manage the missing transport
			} else {
				spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
				rfcomm_emit_volume_changed(rfcomm, -1, SPA_BT_VOLUME_INVALID);
			}
		}
	} else if (spa_streq(buf, "")) {
		/* No commands, reply OK (ITU-T Rec. V.250 Sec. 5.2.1 & 5.6) */
		rfcomm_send_reply(rfcomm, "OK");
	} else if (!rfcomm->slc_configured) {
		spa_log_warn(backend->log, "RFCOMM receive command before SLC completed: %s", buf);
		rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
		return true;

	/* *****
	 * Following commands requires a Service Level Connection
	 * ***** */

	} else if (sscanf(buf, "AT+BCS=%u", &selected_codec) == 1) {
		/* parse BCS(=Bluetooth Codec Selection) reply */
		bool was_switching_codec = rfcomm->hfp_ag_switching_codec && (rfcomm->device != NULL);
		const struct media_codec *codec = codec_list_get(backend, &rfcomm->supported_codec_list, selected_codec);

		rfcomm->hfp_ag_switching_codec = false;
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_NONE;
		codec_switch_stop_timer(rfcomm);
		volume_sync_stop_timer(rfcomm);

		if (!codec) {
			spa_log_warn(backend->log, "unsupported codec negotiation: %d", selected_codec);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			if (was_switching_codec)
				spa_bt_device_emit_codec_switched(rfcomm->device, -EIO);
			return true;
		}

		rfcomm->codec = selected_codec;

		spa_log_debug(backend->log, "RFCOMM selected_codec = %i", selected_codec);

		/* Recreate transport, since previous connection may now be invalid */
		if (rfcomm_new_transport(rfcomm, selected_codec) < 0) {
			// TODO: We should manage the missing transport
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			if (was_switching_codec)
				spa_bt_device_emit_codec_switched(rfcomm->device, -ENOMEM);
			return true;
		}
		spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
		rfcomm_emit_volume_changed(rfcomm, -1, SPA_BT_VOLUME_INVALID);

		rfcomm_send_reply(rfcomm, "OK");
		if (was_switching_codec)
			spa_bt_device_emit_codec_switched(rfcomm->device, 0);
	} else if (spa_strstartswith(buf, "AT+BCC")) {
		if (!rfcomm->codec_negotiation_supported)
			return false;

		rfcomm_send_reply(rfcomm, "OK");
		rfcomm_send_reply(rfcomm, "+BCS: %u", rfcomm->codec);
		rfcomm->hfp_ag_switching_codec = true;
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_NONE;
		codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_TIMEOUT_MSEC);
	} else if (spa_strstartswith(buf, "AT+BIA=")) {
		/* retrieve indicators activation
		 * form: AT+BIA=[indrep1],[indrep2],[indrepx] */
		char *str = buf + 7;
		unsigned int ind = 1;

		while (*str && ind < CIND_MAX && *str != '\r' && *str != '\n') {
			if (*str == ',') {
				ind++;
				goto next_indicator;
			}

			/* Ignore updates to mandantory indicators which are always ON */
			if (ind == CIND_CALL || ind == CIND_CALLSETUP || ind == CIND_CALLHELD)
				goto next_indicator;

			switch (*str) {
			case '0':
				rfcomm->cind_enabled_indicators &= ~(1 << ind);
				break;
			case '1':
				rfcomm->cind_enabled_indicators |= (1 << ind);
				break;
			default:
				spa_log_warn(backend->log, "Unsupported entry in %s: %c", buf, *str);
			}
next_indicator:
			str++;
		}

		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CCWA=")) {
		/*
		 * Claim that call waiting notifications are supported.
		 * Required for some devices (e.g. Soundcore Motion 300),
		 * as they stop sending commands if the reply to CCWA is not OK.
		 */
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CLCC")) {
		struct spa_list *calls;
		struct call *call;
		unsigned int type;

		if (backend->modemmanager) {
			calls = mm_get_calls(backend->modemmanager);
			spa_list_for_each(call, calls, link) {
				if (!call->number) {
					rfcomm_send_reply(rfcomm, "+CLCC: %u,%u,%u,0,%u", call->index, call->direction, call->state, call->multiparty);
				} else {
					if (spa_strstartswith(call->number, "+"))
						type = INTERNATIONAL_NUMBER;
					else
						type = NATIONAL_NUMBER;
					rfcomm_send_reply(rfcomm, "+CLCC: %u,%u,%u,0,%u,\"%s\",%d", call->index, call->direction, call->state,
									call->multiparty, call->number, type);
				}
			}
		}

		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+CLIP=%u", &value) == 1) {
		if (value > 1) {
			spa_log_debug(backend->log, "Unsupported AT+CLIP value: %u", value);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			return true;
		}

		rfcomm->clip_notify = value;
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+CMEE=%u", &value) == 1) {
		if (value > 1) {
			spa_log_debug(backend->log, "Unsupported AT+CMEE value: %u", value);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			return true;
		}

		rfcomm->extended_error_reporting = value;
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CNUM")) {
		if (backend->modem.own_number) {
			unsigned int type;
			if (spa_strstartswith(backend->modem.own_number, "+"))
				type = INTERNATIONAL_NUMBER;
			else
				type = NATIONAL_NUMBER;
			rfcomm_send_reply(rfcomm, "+CNUM: ,\"%s\",%u,,4", backend->modem.own_number, type);
		}
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+COPS=")) {
		unsigned int mode, val;

		if (sscanf(buf, "AT+COPS=%u,%u", &mode, &val) != 2 ||
		      mode != 3 || val != 0) {
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
		} else {
			rfcomm_send_reply(rfcomm, "OK");
		}
	} else if (spa_strstartswith(buf, "AT+COPS?")) {
		if (!backend->modem.network_has_service) {
			rfcomm_send_error(rfcomm, CMEE_NO_NETWORK_SERVICE);
		} else {
			if (backend->modem.operator_name)
				rfcomm_send_reply(rfcomm, "+COPS: 0,0,\"%s\"", backend->modem.operator_name);
			else
				rfcomm_send_reply(rfcomm, "+COPS: 0,,");
			rfcomm_send_reply(rfcomm, "OK");
		}
	} else if (sscanf(buf, "AT+VGM=%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			if (!rfcomm->broken_mic_hw_volume)
				rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", buf);
			rfcomm_send_error(rfcomm, CMEE_OPERATION_NOT_ALLOWED);
		}
	} else if (sscanf(buf, "AT+VGS=%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", buf);
			rfcomm_send_error(rfcomm, CMEE_OPERATION_NOT_ALLOWED);
		}
	} else if (spa_strstartswith(buf, "AT+BIND=?")) {
		rfcomm_send_reply(rfcomm, "+BIND: (2)");
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+BIND?")) {
		rfcomm_send_reply(rfcomm, "+BIND: 1,0");
		rfcomm_send_reply(rfcomm, "+BIND: 2,1");
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+BIND=")) {
		// BIND=... should return a comma separated list of indicators and
		// 2 should be among the other numbers telling that battery charge
		// is supported
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+BIEV=%u,%u", &indicator, &indicator_value) == 2) {
		process_hfp_hf_indicator(rfcomm, indicator, indicator_value);
	} else if (sscanf(buf, "AT+XAPL=%04x-%04x-%*[^,],%u", &xapl_vendor, &xapl_product, &xapl_features) == 3) {
		if (xapl_features & SPA_BT_HFP_HF_XAPL_FEATURE_BATTERY_REPORTING) {
			/* claim, that we support battery status reports */
			rfcomm_send_reply(rfcomm, "+XAPL=iPhone,%u", SPA_BT_HFP_HF_XAPL_FEATURE_BATTERY_REPORTING);
		}
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+XEVENT=USER-AGENT")) {
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+XEVENT=BATTERY,%u,%u,%*u,%*u", &xevent_level, &xevent_nlevels) == 2) {
		process_xevent_indicator(rfcomm, xevent_level, xevent_nlevels);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+XEVENT=BATTERY,%u", &xevent_level) == 1) {
		process_xevent_indicator(rfcomm, xevent_level + 1, 11);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+IPHONEACCEV=%u%n", &count, &r) == 1) {
		if (count < 1 || count > 100)
			return false;

		buf += r;

		for (unsigned int i = 0; i < count; i++) {
			unsigned int key, value;

			if (sscanf(buf, " , %u , %u%n", &key, &value, &r) != 2)
				return false;

			process_iphoneaccev_indicator(rfcomm, key, value);
			buf += r;
		}
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+APLSIRI?")) {
		// This command is sent when we activate Apple extensions
		rfcomm_send_reply(rfcomm, "OK");
	} else if (!mm_is_available(backend->modemmanager)) {
		spa_log_info(backend->log, "RFCOMM receive command but modem not available: %s", buf);
		rfcomm_send_error(rfcomm, CMEE_NO_CONNECTION_TO_PHONE);
		return true;

	/* *****
	 * Following commands requires a Service Level Connection
	 * and access to a modem
	 * ***** */

	} else if (!backend->modem.network_has_service) {
		spa_log_warn(backend->log, "RFCOMM receive command but network not available: %s", buf);
		rfcomm_send_error(rfcomm, CMEE_NO_NETWORK_SERVICE);
		return true;

	/* *****
	 * Following commands requires a Service Level Connection,
	 * access to a modem and to the network
	 * ***** */

	} else if (spa_strstartswith(buf, "ATA")) {
		enum cmee_error error;

		if (!mm_answer_call(backend->modemmanager, rfcomm, &error)) {
			rfcomm_send_error(rfcomm, error);
			return true;
		}
	} else if (spa_strstartswith(buf, "ATD")) {
		char number[31], sep;
		enum cmee_error error;

		if (sscanf(buf, "ATD%30[^;]%c", number, &sep) != 2 || sep != ';') {
			spa_log_debug(backend->log, "Failed to parse ATD: \"%s\"", buf);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			return true;
		}

		if (!mm_do_call(backend->modemmanager, number, rfcomm, &error)) {
			rfcomm_send_error(rfcomm, error);
			return true;
		}
	} else if (spa_strstartswith(buf, "AT+BLDN") && backend->pts) {
		enum cmee_error error;

		/* For PTS tests HFP/AG/OCL/BV-01-C and HFP/AG/OCL/BV-02-C, fake last dial
		 * number by calling first memory */
		if (!mm_do_call(backend->modemmanager, ">1", rfcomm, &error)) {
			rfcomm_send_error(rfcomm, error);
			return true;
		}
	} else if (spa_strstartswith(buf, "AT+CHUP")) {
		enum cmee_error error;

		if (!mm_hangup_call(backend->modemmanager, rfcomm, &error)) {
			rfcomm_send_error(rfcomm, error);
			return true;
		}
	} else if (spa_strstartswith(buf, "AT+VTS=")) {
		char dtmf[2];
		enum cmee_error error;

		if (sscanf(buf, "AT+VTS=%1s", dtmf) != 1) {
			spa_log_debug(backend->log, "Failed to parse AT+VTS: \"%s\"", buf);
			rfcomm_send_error(rfcomm, CMEE_AG_FAILURE);
			return true;
		}

		if (!mm_send_dtmf(backend->modemmanager, dtmf, rfcomm, &error)) {
			rfcomm_send_error(rfcomm, error);
			return true;
		}
	} else {
		return false;
	}

	return true;
}

static void hfp_hf_answer(void *data, DBusMessage *m)
{
	struct rfcomm_call_data *call_data = data;
	struct rfcomm *rfcomm = call_data->rfcomm;
	struct impl *backend = rfcomm->backend;

	if (call_data->call->state != CALL_STATE_INCOMING) {
		telephony_send_dbus_method_reply(backend->telephony, m, BT_TELEPHONY_ERROR_INVALID_STATE, 0);
		return;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "ATA");
}

static void hfp_hf_hangup(void *data, DBusMessage *m)
{
	struct rfcomm_call_data *call_data = data;
	struct rfcomm *rfcomm = call_data->rfcomm;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_held = false;

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_HELD)
			found_held = true;
	}

	switch (call_data->call->state) {
	case CALL_STATE_ACTIVE:
	case CALL_STATE_DIALING:
	case CALL_STATE_ALERTING:
	case CALL_STATE_INCOMING:
		if (found_held) {
			if (!rfcomm->chld_supported) {
				err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
				goto error;
			} else if (rfcomm->hfp_hf_in_progress) {
				err = BT_TELEPHONY_ERROR_IN_PROGRESS;
				goto error;
			}

			rfcomm_send_cmd(rfcomm, hfp_hf_chld1_hangup, m, "AT+CHLD=1");
			rfcomm->hfp_hf_in_progress = true;
		} else {
			rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHUP");
		}
		break;
	case CALL_STATE_HELD:
	case CALL_STATE_WAITING:
		if (rfcomm->hfp_hf_in_progress) {
			err = BT_TELEPHONY_ERROR_IN_PROGRESS;
			goto error;
		}
		rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=0");
		rfcomm->hfp_hf_in_progress = true;
		break;
	default:
		spa_log_warn(backend->log, "Call in invalid state: skip hangup");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	return;
error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static const struct spa_bt_telephony_call_callbacks telephony_call_callbacks = {
	SPA_VERSION_BT_TELEPHONY_AG_CALLBACKS,
	.answer = hfp_hf_answer,
	.hangup = hfp_hf_hangup,
};

static struct spa_bt_telephony_call *hfp_hf_add_call(struct rfcomm *rfcomm, struct spa_bt_telephony_ag *ag, enum spa_bt_telephony_call_state state,
                                                     const char *number)
{
	struct spa_bt_telephony_call *call;
	struct rfcomm_call_data *data;

	call = telephony_call_new(ag, sizeof(*data));
	if (!call)
		return NULL;
	call->state = state;
	if (number)
		call->line_identification = strdup(number);
	data = telephony_call_get_user_data(call);
	data->rfcomm = rfcomm;
	data->call = call;
	telephony_call_set_callbacks(call, &telephony_call_callbacks, data);
	telephony_call_register(call);

	return call;
}

static void hfp_hf_dial(void *data, const char *number, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;

	/* store the number in case we need to create the Call object
	   via CIND notifications (if CLCC is not supported) */
	free(spa_exchange(rfcomm->dialing_number, strdup(number)));

	spa_log_info(backend->log, "Dialing: \"%s\"", number);
	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "ATD%s;", number);
}

static void hfp_hf_swap_calls(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_held = false;

	if (!rfcomm->chld_supported) {
		err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
		goto error;
	} else if (rfcomm->hfp_hf_in_progress) {
		err = BT_TELEPHONY_ERROR_IN_PROGRESS;
		goto error;
	}

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_HELD) {
			found_held = true;
			break;
		}
	}

	if (!found_held) {
		spa_log_debug(backend->log, "no held calls");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=2");
	rfcomm->hfp_hf_in_progress = true;
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_release_and_answer(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_active = false;
	bool found_waiting = false;

	if (!rfcomm->chld_supported) {
		err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
		goto error;
	} else if (rfcomm->hfp_hf_in_progress) {
		err = BT_TELEPHONY_ERROR_IN_PROGRESS;
		goto error;
	}

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_ACTIVE)
			found_active = true;
		else if (call->state == CALL_STATE_WAITING)
			found_waiting = true;
	}

	if (!found_active || !found_waiting) {
		spa_log_debug(backend->log, "no active and waiting calls");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=1");
	rfcomm->hfp_hf_in_progress = true;
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_release_and_swap(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_active = false;
	bool found_held = false;

	if (!rfcomm->chld_supported) {
		err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
		goto error;
	} else if (rfcomm->hfp_hf_in_progress) {
		err = BT_TELEPHONY_ERROR_IN_PROGRESS;
		goto error;
	}

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_WAITING) {
			spa_log_debug(backend->log, "call waiting before release and swap");
			err = BT_TELEPHONY_ERROR_INVALID_STATE;
			goto error;
		} else if (call->state == CALL_STATE_ACTIVE)
			found_active = true;
		else if (call->state == CALL_STATE_HELD)
			found_held = true;
	}

	if (!found_active || !found_held) {
		spa_log_debug(backend->log, "no active and held calls");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=1");
	rfcomm->hfp_hf_in_progress = true;
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_hold_and_answer(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_active = false;
	bool found_waiting = false;

	if (!rfcomm->chld_supported) {
		err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
		goto error;
	} else if (rfcomm->hfp_hf_in_progress) {
		err = BT_TELEPHONY_ERROR_IN_PROGRESS;
		goto error;
	}

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_ACTIVE)
			found_active = true;
		else if (call->state == CALL_STATE_WAITING)
			found_waiting = true;
	}

	if (!found_active || !found_waiting) {
		spa_log_debug(backend->log, "no active and waiting calls");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=2");
	rfcomm->hfp_hf_in_progress = true;
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_hangup_all(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct spa_bt_telephony_call *call;
	bool found_active = false;
	bool found_held = false;

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		switch (call->state) {
		case CALL_STATE_ACTIVE:
		case CALL_STATE_DIALING:
		case CALL_STATE_ALERTING:
		case CALL_STATE_INCOMING:
			found_active = true;
			break;
		case CALL_STATE_HELD:
		case CALL_STATE_WAITING:
			found_held = true;
			break;
		default:
			break;
		}
	}

	/* Hangup held calls */
	if (found_held) {
		rfcomm_send_cmd(rfcomm, hfp_hf_idle, found_active ? NULL : m, "AT+CHLD=0");
	}

	/* Hangup active calls */
	if (found_active) {
		rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHUP");
	}
}

static void hfp_hf_create_multiparty(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found_active = false;
	bool found_held = false;

	if (!rfcomm->chld_supported) {
		err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
		goto error;
	} else if (rfcomm->hfp_hf_in_progress) {
		err = BT_TELEPHONY_ERROR_IN_PROGRESS;
		goto error;
	}

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_WAITING) {
			spa_log_debug(backend->log, "call waiting before creating multiparty");
			err = BT_TELEPHONY_ERROR_INVALID_STATE;
			goto error;
		} else if (call->state == CALL_STATE_ACTIVE)
			found_active = true;
		else if (call->state == CALL_STATE_HELD)
			found_held = true;
	}

	if (!found_active || !found_held) {
		spa_log_debug(backend->log, "no active and held calls");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+CHLD=3");
	rfcomm->hfp_hf_in_progress = true;
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_send_tones(void *data, const char *tones, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;
	struct spa_bt_telephony_call *call;
	bool found = false;

	spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
		if (call->state == CALL_STATE_ACTIVE) {
			found = true;
			break;
		}
	}

	if (!found) {
		spa_log_debug(backend->log, "no active call");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto error;
	}

	rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+VTS=%s", tones);
	return;

error:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static int sco_do_connect(struct spa_bt_transport *t);

static void hfp_hf_transport_activate(void *data, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct impl *backend = rfcomm->backend;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_NONE;

	if (rfcomm->transport && rfcomm->transport->fd > 0) {
		spa_log_debug(backend->log, "transport is already active; SCO socket exists");
		err = BT_TELEPHONY_ERROR_INVALID_STATE;
		goto out;
	}

	if (rfcomm->codec_negotiation_supported) {
		rfcomm_send_cmd(rfcomm, hfp_hf_idle, m, "AT+BCC");
		return;
	} else {
		if (!rfcomm->transport || rfcomm->transport->media_codec->id != SPA_BLUETOOTH_AUDIO_CODEC_CVSD) {
			err = BT_TELEPHONY_ERROR_NOT_SUPPORTED;
			goto out;
		}

		sco_do_connect(rfcomm->transport);
	}

out:
	telephony_send_dbus_method_reply(backend->telephony, m, err, 0);
}

static void hfp_hf_set_speaker_volume(void *data, uint8_t volume, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct spa_bt_transport_volume *t_volume;

	rfcomm->volumes[SPA_BT_VOLUME_ID_RX].hw_volume = volume;
	if (rfcomm_hw_volume_enabled(rfcomm)) {
		t_volume = rfcomm->transport ? &rfcomm->transport->volumes[SPA_BT_VOLUME_ID_RX] : NULL;

		if (t_volume && t_volume->active) {
			t_volume->volume = (float) spa_bt_volume_hw_to_linear(volume, t_volume->hw_volume_max);
			spa_bt_transport_emit_volume_changed(rfcomm->transport);
		}
	}

	rfcomm_send_volume_cmd(rfcomm, hfp_hf_idle, m, SPA_BT_VOLUME_ID_RX);
}

static void hfp_hf_set_microphone_volume(void *data, uint8_t volume, DBusMessage *m)
{
	struct rfcomm *rfcomm = data;
	struct spa_bt_transport_volume *t_volume;

	rfcomm->volumes[SPA_BT_VOLUME_ID_TX].hw_volume = volume;
	if (rfcomm_hw_volume_enabled(rfcomm)) {
		t_volume = rfcomm->transport ? &rfcomm->transport->volumes[SPA_BT_VOLUME_ID_TX] : NULL;

		if (t_volume && t_volume->active) {
			t_volume->volume = (float) spa_bt_volume_hw_to_linear(volume, t_volume->hw_volume_max);
			spa_bt_transport_emit_volume_changed(rfcomm->transport);
		}
	}

	rfcomm_send_volume_cmd(rfcomm, hfp_hf_idle, m, SPA_BT_VOLUME_ID_TX);
}

static const struct spa_bt_telephony_ag_callbacks telephony_ag_callbacks = {
	SPA_VERSION_BT_TELEPHONY_AG_CALLBACKS,
	.dial = hfp_hf_dial,
	.swap_calls = hfp_hf_swap_calls,
	.release_and_answer = hfp_hf_release_and_answer,
	.release_and_swap = hfp_hf_release_and_swap,
	.hold_and_answer = hfp_hf_hold_and_answer,
	.hangup_all = hfp_hf_hangup_all,
	.create_multiparty = hfp_hf_create_multiparty,
	.send_tones = hfp_hf_send_tones,
	.transport_activate = hfp_hf_transport_activate,
	.set_speaker_volume = hfp_hf_set_speaker_volume,
	.set_microphone_volume = hfp_hf_set_microphone_volume,
};

#define hfp_hf_set_call_state(log, obj, new_state) \
({ \
	spa_log_debug(log, "call id: %u, %u -> %u", obj->id, obj->state, new_state); \
	obj->state = new_state; \
})

static void hfp_hf_remove_disconnected_calls(struct rfcomm *rfcomm)
{
	struct impl *backend = rfcomm->backend;
	struct spa_bt_telephony_call *call, *call_tmp;
	struct updated_call *updated_call;
	bool found;

	spa_list_for_each_safe(call, call_tmp, &rfcomm->telephony_ag->call_list, link) {
		found = false;
		spa_list_for_each(updated_call, &rfcomm->updated_call_list, link) {
			if (call->id == updated_call->id) {
				found = true;
				break;
			}
		}

		spa_log_debug(backend->log, "call %d -> %s", call->id, found ? "updated" : "disconnected");

		if (!found) {
			hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
			telephony_call_notify_updated_props(call);
			telephony_call_destroy(call);
		}
	}

	spa_list_consume(updated_call, &rfcomm->updated_call_list, link) {
		spa_list_remove(&updated_call->link);
		free(updated_call);
	}
}

static bool rfcomm_hfp_hf(struct rfcomm *rfcomm, char* token)
{
	struct impl *backend = rfcomm->backend;
	unsigned int features, gain, selected_codec, indicator, value, type;
	char number[17];

	if (sscanf(token, "+BRSF:%u", &features) == 1) {
		if (((features & (SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION)) != 0) &&
				!spa_list_is_empty(&rfcomm->available_codec_list))
			rfcomm->codec_negotiation_supported = true;
		rfcomm->hfp_hf_3way = (features & SPA_BT_HFP_AG_FEATURE_3WAY) != 0;
		rfcomm->hfp_hf_nrec = (features & SPA_BT_HFP_AG_FEATURE_ECNR) != 0;
		rfcomm->hfp_hf_clcc = (features & SPA_BT_HFP_AG_FEATURE_ENHANCED_CALL_STATUS) != 0;
		rfcomm->hfp_hf_cme = (features & SPA_BT_HFP_AG_FEATURE_EXTENDED_RES_CODE) != 0;
	} else if (sscanf(token, "+BCS:%u", &selected_codec) == 1 && rfcomm->codec_negotiation_supported) {
		const struct media_codec *codec = codec_list_get(backend, &rfcomm->available_codec_list, selected_codec);

		if (!codec) {
			spa_log_warn(backend->log, "unsupported codec negotiation: %d", selected_codec);
		} else {
			spa_log_debug(backend->log, "RFCOMM selected_codec = %i", selected_codec);

			/* send codec selection to AG */
			rfcomm_send_cmd(rfcomm, hfp_hf_idle, NULL, "AT+BCS=%u", selected_codec);

			if (!rfcomm->transport || (rfcomm->codec != selected_codec) ) {
				if (rfcomm_new_transport(rfcomm, selected_codec) < 0) {
					// TODO: We should manage the missing transport
				} else {
					spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
				}
			}
		}
	} else if (sscanf(token, "+VGM%*1[:=]%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", token);
		}
	} else if (sscanf(token, "+VGS%*1[:=]%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", token);
		}
	} else if (spa_strstartswith(token, "+CIND: (")) {
		uint8_t i = 1;
		while (strstr(token, "\"")) {
			token += strcspn(token, "\"") + 1;
			token[strcspn(token, "\"")] = 0;
			rfcomm->hf_indicators[i] = strdup(token);
			token += strcspn(token, "\"") + 1;
			i++;
			if (i == MAX_HF_INDICATORS) {
				break;
			}
		}
	} else if (spa_strstartswith(token, "+CIND: ")) {
		token[strcspn(token, "\r")] = 0;
		token[strcspn(token, "\n")] = 0;
		token += strlen("+CIND: ");
		uint8_t i = 1;
		while (strlen(token)) {
			if (i >= MAX_HF_INDICATORS || !rfcomm->hf_indicators[i]) {
				break;
			}
			token[strcspn(token, ",")] = 0;
			spa_log_info(backend->log, "AG indicator state: %s = %i", rfcomm->hf_indicators[i], atoi(token));

			if (spa_streq(rfcomm->hf_indicators[i], "battchg")) {
				spa_bt_device_report_battery_level(rfcomm->device, atoi(token) * 100 / 5);
			}

			token += strcspn(token, "\0") + 1;
			i++;
		}
	} else if (spa_strstartswith(token, "+CHLD: (")) {
		int chlds = 0;
		token[strcspn(token, "\r")] = 0;
		token[strcspn(token, "\n")] = 0;
		token[strcspn(token, ")")] = 0;
		token += strlen("+CHLD: (");
		while (strlen(token)) {
			token[strcspn(token, ",")] = 0;
			if (spa_streq(token, "0"))
				chlds |= 1 << 0;
			else if (spa_streq(token, "1"))
				chlds |= 1 << 1;
			else if (spa_streq(token, "2"))
				chlds |= 1 << 2;
			else if (spa_streq(token, "3"))
				chlds |= 1 << 3;
			token += strcspn(token, "\0") + 1;
		}
		rfcomm->chld_supported = (chlds == 0x0F);
		spa_log_debug(backend->log, "AT+CHLD supported: %d (0x%X)", rfcomm->chld_supported, chlds);
	} else if (sscanf(token, "+CIEV: %u,%u", &indicator, &value) == 2) {
		if (indicator >= MAX_HF_INDICATORS || !rfcomm->hf_indicators[indicator]) {
			spa_log_warn(backend->log, "indicator %u has not been registered, ignoring", indicator);
		} else {
			spa_log_info(backend->log, "AG indicator update: %s = %u", rfcomm->hf_indicators[indicator], value);

			if (spa_streq(rfcomm->hf_indicators[indicator], "battchg")) {
				spa_bt_device_report_battery_level(rfcomm->device, value * 100 / 5);
			} else if (spa_streq(rfcomm->hf_indicators[indicator], "callsetup")) {
				if (rfcomm->hfp_hf_clcc) {
					rfcomm_send_cmd(rfcomm, hfp_hf_clcc_update, NULL, "AT+CLCC");
					return true;
				}

				if (value == CIND_CALLSETUP_NONE) {
					struct spa_bt_telephony_call *call, *tcall;
					spa_list_for_each_safe(call, tcall, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_DIALING || call->state == CALL_STATE_ALERTING ||
						    call->state == CALL_STATE_INCOMING || call->state == CALL_STATE_WAITING) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
							telephony_call_notify_updated_props(call);
							telephony_call_destroy(call);
						}
					}
				} else if (value == CIND_CALLSETUP_INCOMING) {
					struct spa_bt_telephony_call *call;
					bool found = false;

					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_INCOMING || call->state == CALL_STATE_WAITING) {
							spa_log_info(backend->log, "incoming call already in progress (%d)", call->state);
							found = true;
							break;
						}
					}

					if (!found && !rfcomm->hfp_hf_clcc) {
						spa_log_info(backend->log, "Incoming call");
						if (hfp_hf_add_call(rfcomm, rfcomm->telephony_ag, CALL_STATE_INCOMING, NULL) == NULL)
							spa_log_warn(backend->log, "failed to create incoming call");
					}
				} else if (value == CIND_CALLSETUP_DIALING) {
					struct spa_bt_telephony_call *call;
					bool found = false;

					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_DIALING || call->state == CALL_STATE_ALERTING) {
							spa_log_info(backend->log, "dialing call already in progress (%d)", call->state);
							found = true;
							break;
						}
					}

					if (!found && !rfcomm->hfp_hf_clcc) {
						spa_log_info(backend->log, "Dialing call");
						if (hfp_hf_add_call(rfcomm, rfcomm->telephony_ag, CALL_STATE_DIALING, rfcomm->dialing_number) == NULL)
							spa_log_warn(backend->log, "failed to create dialing call");
						spa_clear_ptr(rfcomm->dialing_number, free);
					}
				} else if (value == CIND_CALLSETUP_ALERTING) {
					struct spa_bt_telephony_call *call;
					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_DIALING) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_ALERTING);
							telephony_call_notify_updated_props(call);
						}
					}
				}

				rfcomm->hfp_hf_in_progress = false;
			} else if (spa_streq(rfcomm->hf_indicators[indicator], "call")) {
				if (rfcomm->hfp_hf_clcc) {
					rfcomm_send_cmd(rfcomm, hfp_hf_clcc_update, NULL, "AT+CLCC");
					return true;
				}

				if (value == 0) {
					struct spa_bt_telephony_call *call, *tcall;
					spa_list_for_each_safe(call, tcall, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_ACTIVE) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
							telephony_call_notify_updated_props(call);
							telephony_call_destroy(call);
						}
					}
				} else if (value == 1) {
					struct spa_bt_telephony_call *call;
					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_DIALING || call->state == CALL_STATE_ALERTING ||
						    call->state == CALL_STATE_INCOMING) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_ACTIVE);
							telephony_call_notify_updated_props(call);
						}
					}
				}

				rfcomm->hfp_hf_in_progress = false;
			} else if (spa_streq(rfcomm->hf_indicators[indicator], "callheld")) {
				if (rfcomm->hfp_hf_clcc) {
					rfcomm_send_cmd(rfcomm, hfp_hf_clcc_update, NULL, "AT+CLCC");
					return true;
				}

				if (value == 0) {	/* Reject waiting call or no held calls */
					struct spa_bt_telephony_call *call, *tcall;
					bool found_waiting = false;
					spa_list_for_each_safe(call, tcall, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_WAITING) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
							telephony_call_notify_updated_props(call);
							telephony_call_destroy(call);
							found_waiting = true;
							break;
						}
					}
					if (!found_waiting) {
						spa_list_for_each_safe(call, tcall, &rfcomm->telephony_ag->call_list, link) {
							if (call->state == CALL_STATE_HELD) {
								hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
								telephony_call_notify_updated_props(call);
								telephony_call_destroy(call);
							}
						}
					}
				} else if (value == 1) {	/* Swap calls */
					struct spa_bt_telephony_call *call;
					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						bool changed = false;
						if (call->state == CALL_STATE_ACTIVE) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_HELD);
							changed = true;
						} else if (call->state == CALL_STATE_HELD) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_ACTIVE);
							changed = true;
						}

						if (changed)
							telephony_call_notify_updated_props(call);
					}
				} else if (value == 2) {	/* No active calls, place waiting on hold */
					struct spa_bt_telephony_call *call;
					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						bool changed = false;
						if (call->state == CALL_STATE_ACTIVE || call->state == CALL_STATE_WAITING) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_HELD);
							changed = true;
						}

						if (changed)
							telephony_call_notify_updated_props(call);
					}
				}

				rfcomm->hfp_hf_in_progress = false;
			}
		}
	} else if (sscanf(token, "+CLIP: \"%16[^\"]\",%u", number, &type) == 2) {
		struct spa_bt_telephony_call *call;
		spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
			if (call->state == CALL_STATE_INCOMING && !spa_streq(number, call->line_identification)) {
				if (call->line_identification)
					free(call->line_identification);
				call->line_identification = strdup(number);
				telephony_call_notify_updated_props(call);
				break;
			}
		}
	} else if (sscanf(token, "+CCWA: \"%16[^\"]\",%u", number, &type) == 2) {
		struct spa_bt_telephony_call *call;
		bool found = false;

		spa_log_info(backend->log, "Waiting call");
		spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
			if (call->state == CALL_STATE_WAITING) {
				spa_log_info(backend->log, "waiting call already in progress (id: %d)", call->id);
				found = true;
				break;
			}
		}
		if (!found) {
			call = hfp_hf_add_call(rfcomm, rfcomm->telephony_ag, CALL_STATE_WAITING, number);
			if (call == NULL)
				spa_log_warn(backend->log, "failed to create waiting call");
		}
	} else if (spa_strstartswith(token, "+CLCC:")) {
		struct spa_bt_telephony_call *call;
		size_t pos;
		char *token_end;
		int idx;
		unsigned int status, mpty;
		bool parsed = false, found = false;

		number[0] = '\0';

		token[strcspn(token, "\r")] = 0;
		token[strcspn(token, "\n")] = 0;
		token_end = token + strlen(token);
		token += strlen("+CLCC:");

		if (token < token_end) {
			pos = strcspn(token, ",");
			token[pos] = '\0';
			idx = atoi(token);
			token += pos + 1;
		}
		if (token < token_end) {
			// Skip direction
			pos = strcspn(token, ",");
			token += pos + 1;
		}
		if (token < token_end) {
			pos = strcspn(token, ",");
			token[pos] = '\0';
			status = atoi(token);
			token += pos + 1;
		}
		if (token < token_end) {
			// Skip mode
			pos = strcspn(token, ",");
			token += pos + 1;
		}
		if (token < token_end) {
			pos = strcspn(token, ",");
			token[pos] = '\0';
			mpty = atoi(token);
			token += pos + 1;
			parsed = true;
		}
		if (token < token_end) {
			if (sscanf(token, "\"%16[^\"]\",%u", number, &type) != 2) {
				spa_log_warn(backend->log, "Failed to parse number: %s", token);
				number[0] = '\0';
			}
		}

		if (SPA_LIKELY (parsed)) {
			struct updated_call *updated_call;
			updated_call = calloc(1, sizeof(struct updated_call));
			updated_call->id = idx;
			spa_list_append(&rfcomm->updated_call_list, &updated_call->link);

			spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
				if (call->id == idx) {
					bool changed = false;

					found = true;

					if (call->state != status) {
						call->state =status;
						changed = true;
					}
					if (call->multiparty != mpty) {
						call->multiparty = mpty;
						changed = true;
					}
					if (strlen(number) && !spa_streq(number, call->line_identification)) {
						if (call->line_identification)
							free(call->line_identification);
						call->line_identification = strdup(number);
						changed = true;
					}

					if (changed)
						telephony_call_notify_updated_props(call);
				}
			}

			if (!found) {
				spa_log_info(backend->log, "New call, initial state: %u", status);
				call = hfp_hf_add_call(rfcomm, rfcomm->telephony_ag, status, strlen(number) ? number : NULL);
				if (call == NULL)
					spa_log_warn(backend->log, "failed to create call");
				else if (call->id != idx)
					spa_log_warn(backend->log, "wrong call index: %d, expected: %d", call->id, idx);

				if (spa_streq(number, rfcomm->dialing_number)) {
					free(rfcomm->dialing_number);
					rfcomm->dialing_number = NULL;
				}
			}
		} else {
			spa_log_warn(backend->log, "malformed +CLCC command received from AG");
		}
	} else if (spa_strstartswith(token, "OK") || spa_strstartswith(token, "ERROR") ||
				spa_strstartswith(token, "+CME ERROR:")) {

		rfcomm_cmd_done(rfcomm, token);

		if (spa_strstartswith(token, "OK")) {
			switch(rfcomm->hf_state) {
			case hfp_hf_brsf:
				if (rfcomm->codec_negotiation_supported) {
					char buf[64];
					struct spa_strbuf str;
					struct codec_item *item;

					spa_strbuf_init(&str, buf, sizeof(buf));
					spa_strbuf_append(&str, "1");
					spa_list_for_each(item, &rfcomm->available_codec_list, link)
						spa_strbuf_append(&str, ",%u", item->codec->codec_id);

					rfcomm_send_cmd(rfcomm, hfp_hf_bac, NULL, "AT+BAC=%s", buf);
				} else {
					rfcomm_send_cmd(rfcomm, hfp_hf_cind1, NULL, "AT+CIND=?");
				}
				break;
			case hfp_hf_bac:
				rfcomm_send_cmd(rfcomm, hfp_hf_cind1, NULL, "AT+CIND=?");
				break;
			case hfp_hf_cind1:
				rfcomm_send_cmd(rfcomm, hfp_hf_cind2, NULL, "AT+CIND?");
				break;
			case hfp_hf_cind2:
				rfcomm_send_cmd(rfcomm, hfp_hf_cmer, NULL, "AT+CMER=3,0,0,1");
				break;
			case hfp_hf_cmer:
				if (rfcomm->hfp_hf_3way) {
					rfcomm_send_cmd(rfcomm, hfp_hf_chld, NULL, "AT+CHLD=?");
					break;
				}
				SPA_FALLTHROUGH;
			case hfp_hf_chld:
				rfcomm->slc_configured = true;

				if (!rfcomm->codec_negotiation_supported) {
					if (rfcomm_new_transport(rfcomm, HFP_AUDIO_CODEC_CVSD) < 0) {
						// TODO: We should manage the missing transport
					} else {
						spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
					}
				}

				rfcomm->telephony_ag = telephony_ag_new(backend->telephony, 0);
				rfcomm->telephony_ag->address = strdup(rfcomm->device->address);
				rfcomm->telephony_ag->volume[SPA_BT_VOLUME_ID_RX] = rfcomm->volumes[SPA_BT_VOLUME_ID_RX].hw_volume = backend->hfp_default_speaker_volume;
				rfcomm->telephony_ag->volume[SPA_BT_VOLUME_ID_TX] = rfcomm->volumes[SPA_BT_VOLUME_ID_TX].hw_volume = backend->hfp_default_mic_volume;
				telephony_ag_set_callbacks(rfcomm->telephony_ag,
							&telephony_ag_callbacks, rfcomm);
				if (rfcomm->transport) {
					rfcomm->telephony_ag->transport.codec = rfcomm->transport->media_codec->codec_id;
					rfcomm->telephony_ag->transport.state = rfcomm->transport->state;
				}
				telephony_ag_register(rfcomm->telephony_ag);

				rfcomm_send_cmd(rfcomm, hfp_hf_clip, NULL, "AT+CLIP=1");
				break;
			case hfp_hf_clip:
				if (rfcomm->chld_supported) {
					rfcomm_send_cmd(rfcomm, hfp_hf_ccwa, NULL, "AT+CCWA=1");
					break;
				}
				SPA_FALLTHROUGH;
			case hfp_hf_ccwa:
				if (rfcomm->hfp_hf_cme) {
					rfcomm_send_cmd(rfcomm, hfp_hf_cmee, NULL, "AT+CMEE=1");
					break;
				}
				SPA_FALLTHROUGH;
			case hfp_hf_cmee:
				if (backend->hfp_disable_nrec && rfcomm->hfp_hf_nrec) {
					rfcomm_send_cmd(rfcomm, hfp_hf_nrec, NULL, "AT+NREC=0");
					break;
				}
				SPA_FALLTHROUGH;
			case hfp_hf_nrec:
				if (rfcomm->hfp_hf_clcc) {
					rfcomm_send_cmd(rfcomm, hfp_hf_clcc, NULL, "AT+CLCC");
					break;
				} else {
					// TODO: Create calls if CIND reports one during SLC setup
				}

				/* Report volume on SLC establishment */
				SPA_FALLTHROUGH;
			case hfp_hf_clcc:
				if (rfcomm->hf_state == hfp_hf_clcc) {
					hfp_hf_remove_disconnected_calls(rfcomm);
				}
				rfcomm_send_volume_cmd(rfcomm, hfp_hf_vgs, NULL, SPA_BT_VOLUME_ID_RX);
				break;
			case hfp_hf_vgs:
				rfcomm_send_volume_cmd(rfcomm, hfp_hf_idle, NULL, SPA_BT_VOLUME_ID_TX);
				break;
			case hfp_hf_clcc_update:
				hfp_hf_remove_disconnected_calls(rfcomm);
				rfcomm->hfp_hf_in_progress = false;
				break;
			case hfp_hf_chld1_hangup:
				/* For HFP/HF/TWC/BV-03-C - see 0e92ab9307e05758b3f70b4c0648e29c1d1e50be */
				if (!rfcomm->hfp_hf_clcc) {
					struct spa_bt_telephony_call *call, *tcall;
					spa_list_for_each_safe(call, tcall, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_ACTIVE) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_DISCONNECTED);
							telephony_call_notify_updated_props(call);
							telephony_call_destroy(call);
						}
					}
					spa_list_for_each(call, &rfcomm->telephony_ag->call_list, link) {
						if (call->state == CALL_STATE_HELD) {
							hfp_hf_set_call_state(backend->log, call, CALL_STATE_ACTIVE);
							telephony_call_notify_updated_props(call);
						}
					}
				}
				break;
			case hfp_hf_idle:
			default:
				break;
			}
		} else {
			/* reset state in case of an error reply */
			rfcomm->hfp_hf_in_progress = false;
		}

		rfcomm_send_next_cmd(rfcomm);
	}

	return true;
}

#endif

static void rfcomm_process_events(struct rfcomm *rfcomm, char *buf, bool ag, bool (*handler)(struct rfcomm *, char *))
{
	struct impl *backend = rfcomm->backend;
	char *token;

	/* Relaxed parsing of both <COMMAND>\r (AG) and \r\n<REPLY>\r\n (HF) */

	while ((token = strsep(&buf, "\r"))) {
		size_t len;

		/* Skip leading and trailing \n */
		while (*token == '\n')
			++token;
		for (len = strlen(token); len > 0 && token[len - 1] == '\n'; --len)
			token[len - 1] = '\0';

		/* Skip empty (only last one if AG) */
		if (*token == '\0' && (buf == NULL || !ag))
			continue;

		spa_log_debug(backend->log, "RFCOMM event: %s", token);

		if (!handler(rfcomm, token)) {
			spa_log_debug(backend->log, "RFCOMM received unsupported event: %s", token);
			if (ag)
				rfcomm_send_error(rfcomm, CMEE_OPERATION_NOT_SUPPORTED);
		}
	}
}

static void rfcomm_event(struct spa_source *source)
{
	struct rfcomm *rfcomm = source->data;
	struct impl *backend = rfcomm->backend;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_info(backend->log, "lost RFCOMM connection.");
		rfcomm_free(rfcomm);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		char buf[512];
		ssize_t len;

		len = read(source->fd, buf, sizeof(buf) - 1);
		if (len < 0) {
			spa_log_error(backend->log, "RFCOMM read error: %s", strerror(errno));
			return;
		}
		buf[len] = 0;

		spa_log_debug(backend->log, "RFCOMM << %s", buf);
		spa_debug_log_mem(backend->log, SPA_LOG_LEVEL_DEBUG, 2, buf, strlen(buf));

		switch (rfcomm->profile) {
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
		case SPA_BT_PROFILE_HSP_HS:
			rfcomm_process_events(rfcomm, buf, true, rfcomm_hsp_ag);
			break;
		case SPA_BT_PROFILE_HSP_AG:
			rfcomm_process_events(rfcomm, buf, false, rfcomm_hsp_hs);
			break;
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
		case SPA_BT_PROFILE_HFP_HF:
			rfcomm_process_events(rfcomm, buf, true, rfcomm_hfp_ag);
			break;
		case SPA_BT_PROFILE_HFP_AG:
			rfcomm_process_events(rfcomm, buf, false, rfcomm_hfp_hf);
			break;
#endif
		default:
			break;
		}
	}
}

static int sco_create_socket(struct impl *backend, struct spa_bt_adapter *adapter, bool transparent)
{
	struct sockaddr_sco addr;
	socklen_t len;
	bdaddr_t src;
	uint32_t bt_features;

	spa_autoclose int sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, "socket(SEQPACKET, SCO) %s", strerror(errno));
		return -1;
	}

	str2ba(adapter->address, &src);

	len = sizeof(addr);
	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, len) < 0) {
		spa_log_error(backend->log, "bind(): %s", strerror(errno));
		return -1;
	}

	spa_log_debug(backend->log, "transparent=%d", (int)transparent);
	if (transparent) {
		/* set correct socket options for mSBC/LC3 */
		struct bt_voice voice_config;
		memset(&voice_config, 0, sizeof(voice_config));
		voice_config.setting = BT_VOICE_TRANSPARENT;
		if (setsockopt(sock, SOL_BLUETOOTH, BT_VOICE, &voice_config, sizeof(voice_config)) < 0) {
			spa_log_error(backend->log, "setsockopt(): %s", strerror(errno));
			return -1;
		}
	}

	sco_offload_btcodec(backend, sock, transparent);

	return spa_steal_fd(sock);
}

static int sco_do_connect(struct spa_bt_transport *t)
{
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	struct spa_bt_device *d = t->device;
	struct transport_data *td = t->user_data;
	struct sockaddr_sco addr;
	int err;

	spa_log_debug(backend->log, "transport %p: enter sco_do_connect, codec=%s",
			t, t->media_codec->description);

	td->err = -EIO;

	if (d->adapter == NULL)
		return -EIO;

	spa_zero(addr);
	addr.sco_family = AF_BLUETOOTH;
	str2ba(d->address, &addr.sco_bdaddr);

	for (int retry = 2;;) {
		bool encoded = t->media_codec->id != SPA_BLUETOOTH_AUDIO_CODEC_CVSD;
		spa_autoclose int sock = sco_create_socket(backend, d->adapter, encoded);
		if (sock < 0)
			return -1;

		spa_log_debug(backend->log, "transport %p: doing connect", t);
		err = connect(sock, (struct sockaddr *) &addr, sizeof(addr));
		if (err < 0 && errno == ECONNABORTED && retry-- > 0) {
			spa_log_warn(backend->log, "connect(): %s. Remaining retry:%d",
					strerror(errno), retry);
			continue;
		} else if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
			spa_log_error(backend->log, "connect(): %s", strerror(errno));
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
			if (errno == EOPNOTSUPP && encoded &&
					td->rfcomm->codec_negotiation_supported) {
				/* Adapter doesn't support msbc/lc3/etc. Renegotiate. */
				d->adapter->msbc_probed = true;
				d->adapter->has_msbc = false;

				codec_list_clear(&td->rfcomm->available_codec_list);
				codec_list_clear(&td->rfcomm->supported_codec_list);

				if (t->profile == SPA_BT_PROFILE_HFP_HF) {
					td->rfcomm->hfp_ag_switching_codec = true;
					rfcomm_send_reply(td->rfcomm, "+BCS: 1");
				} else if (t->profile == SPA_BT_PROFILE_HFP_AG) {
					rfcomm_send_cmd(td->rfcomm, hfp_hf_idle, NULL, "AT+BAC=1");
				}
			}
#endif
			return -1;
		}

		td->err = -EINPROGRESS;

		return spa_steal_fd(sock);
	}
}

static int rfcomm_ag_sync_volume(struct rfcomm *rfcomm, bool later);

static void sco_ready(struct spa_bt_transport *t)
{
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	struct transport_data *td = t->user_data;
	struct sco_options sco_opt;
	socklen_t len;
	int err;

	spa_log_debug(backend->log, "transport %p: ready", t);

	/* Read socket error status */
	if (t->fd >= 0) {
		if (td->err == -EINPROGRESS) {
			len = sizeof(err);
			memset(&err, 0, len);
			if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
				td->err = -errno;
			else
				td->err = -err;
		}
	} else {
		td->err = -EIO;
	}

	if (!td->requesting)
		return;

	td->requesting = false;

	if (td->err)
		goto done;

	/* XXX: The MTU as currently reported by kernel (6.2) here is not a valid packet size,
	 * XXX: for USB adapters, see sco-io.
	 */
	len = sizeof(sco_opt);
	memset(&sco_opt, 0, len);
	if (getsockopt(t->fd, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0) {
		spa_log_warn(backend->log, "getsockopt(SCO_OPTIONS) failed: %d (%m)", errno);
		t->read_mtu = 144;
		t->write_mtu = 144;
	} else {
		spa_log_debug(backend->log, "autodetected mtu = %u", sco_opt.mtu);
		t->read_mtu = sco_opt.mtu;
		t->write_mtu = sco_opt.mtu;
	}

	/* Clear nonblocking flag we set for connect() */
	err = fcntl(t->fd, F_GETFL, O_NONBLOCK);
	if (err < 0) {
		td->err = -errno;
		goto done;
	}
	err &= ~O_NONBLOCK;
	err = fcntl(t->fd, F_SETFL, O_NONBLOCK, err);
	if (err < 0) {
		td->err = -errno;
		goto done;
	}

done:
	if (td->err) {
		spa_log_debug(backend->log, "transport %p: acquire failed: %s (%d)",
				t, strerror(-td->err), td->err);
		spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_ERROR);
		return;
	}

	spa_log_debug(backend->log, "transport %p: acquire complete, read_mtu=%u, write_mtu=%u",
			t, t->read_mtu, t->write_mtu);

	/*
	 * Send RFCOMM volume after connection is ready, and also after
	 * a timeout.
	 *
	 * Some headsets adjust their HFP volume when in A2DP mode
	 * without reporting via RFCOMM to us, so the volume level can
	 * be out of sync, and we can't know what it is. Moreover, they may
	 * take the first +VGS command after connection only partially
	 * into account, and need a long enough timeout.
	 *
	 * E.g. with Sennheiser HD-250BT, the first +VGS changes the
	 * actual volume, but does not update the level in the hardware
	 * volume buttons, which is updated by an +VGS event only after
	 * sufficient time is elapsed from the connection.
	 */
	rfcomm_ag_sync_volume(td->rfcomm, false);
	rfcomm_ag_sync_volume(td->rfcomm, true);

	spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_ACTIVE);
}

static void sco_start_source(struct spa_bt_transport *t);

static int sco_acquire_cb(void *data, bool optional)
{
	struct spa_bt_transport *t = data;
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	int sock;

	spa_log_debug(backend->log, "transport %p: enter sco_acquire_cb", t);

	if (optional || t->fd > 0)
		sock = t->fd;
	else
		sock = sco_do_connect(t);

	if (sock < 0)
		goto fail;

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (!td->rfcomm->device->disable_dummy_call)
		rfcomm_hfp_ag_set_cind(td->rfcomm, true);
#endif

	t->fd = sock;

	td->requesting = true;

	sco_start_source(t);

	if (td->err != -EINPROGRESS)
		sco_ready(t);

	return 0;

fail:
	spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_ERROR);
	return -1;
}

static int sco_destroy_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);

	if (t->sco_io) {
		spa_bt_sco_io_destroy(t->sco_io);
		t->sco_io = NULL;
	}

	if (td->sco.loop)
		spa_loop_remove_source(backend->main_loop, &td->sco);

	if (t->fd > 0) {
		/* Shutdown and close the socket */
		shutdown(t->fd, SHUT_RDWR);
		close(t->fd);
		t->fd = -1;
	}

	return 0;
}

static int sco_release_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);

	spa_log_info(backend->log, "Transport %s released", t->path);

	spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_IDLE);

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (!td->rfcomm->device->disable_dummy_call)
		rfcomm_hfp_ag_set_cind(td->rfcomm, false);
#endif

	sco_destroy_cb(t);

	return 0;
}

static void sco_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		/* sco_ready() reads the socket error status in td->err */
		sco_ready(t);

		if (td->err < 0) {
			spa_log_info(backend->log, "transport %p: SCO socket error: %s (%d)",
					t, strerror(-td->err), td->err);
		} else {
			spa_log_debug(backend->log, "transport %p: SCO socket hangup", t);
		}

		if (source->loop)
			spa_loop_remove_source(source->loop, source);
		if (t->fd >= 0) {
			spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_IDLE);
			shutdown(t->fd, SHUT_RDWR);
			close(t->fd);
			t->fd = -1;
		}
	}

	if (source->rmask & (SPA_IO_OUT | SPA_IO_IN)) {
		SPA_FLAG_CLEAR(source->mask, SPA_IO_OUT | SPA_IO_IN);
		spa_loop_update_source(backend->main_loop, source);
		sco_ready(t);
	}
}

static void sco_start_source(struct spa_bt_transport *t)
{
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	struct transport_data *td = t->user_data;

	if (td->sco.loop)
		return;

	td->err = -EINPROGRESS;

	td->sco.func = sco_event;
	td->sco.data = t;
	td->sco.fd = t->fd;
	td->sco.mask = SPA_IO_HUP | SPA_IO_ERR;
	td->sco.rmask = 0;

	switch (t->device->adapter->bus_type) {
	case BUS_TYPE_USB:
		/* With USB controllers, we have to determine packet size from incoming
		 * packets before we can send. Wait for POLLIN when connecting (not
		 * POLLOUT as usual).
		 */
		td->sco.mask |= SPA_IO_IN;
		break;
	default:
		td->sco.mask |= SPA_IO_OUT;
		break;
	}

	spa_loop_add_source(backend->main_loop, &td->sco);
}

static void sco_listen_event(struct spa_source *source)
{
	struct impl *backend = source->data;
	struct sockaddr_sco addr;
	socklen_t addrlen;
	char local_address[18], remote_address[18];
	struct rfcomm *rfcomm;
	struct spa_bt_transport *t = NULL;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_error(backend->log, "error listening SCO connection: %s", strerror(errno));
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	spa_log_debug(backend->log, "doing accept");
	spa_autoclose int sock = accept(source->fd, (struct sockaddr *) &addr, &addrlen);
	if (sock < 0) {
		if (errno != EAGAIN)
			spa_log_error(backend->log, "SCO accept(): %s", strerror(errno));
		return;
	}

	ba2str(&addr.sco_bdaddr, remote_address);

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if (getsockname(sock, (struct sockaddr *) &addr, &addrlen) < 0) {
		spa_log_error(backend->log, "SCO getsockname(): %s", strerror(errno));
		return;
	}

	ba2str(&addr.sco_bdaddr, local_address);

	/* Find transport for local and remote address */
	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		/* Audio connection is allowed from both side with legacy peer, i.e. HSP or codec negotion not supported
		 * (except if PTS workaround has been enabled in which case audio coonection is allowed as for HSP),
		 * or only from the HFP Audio Gateway. */
		if ((((!rfcomm->codec_negotiation_supported || backend->pts) && (rfcomm->profile & SPA_BT_PROFILE_HEADSET_AUDIO)) ||
				(rfcomm->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)) &&
				rfcomm->transport &&
				spa_streq(rfcomm->device->address, remote_address) &&
				spa_streq(rfcomm->device->adapter->address, local_address)) {
			t = rfcomm->transport;
			break;
		}
	}
	if (!t) {
		spa_log_debug(backend->log, "No transport for adapter %s and remote %s",
		              local_address, remote_address);
		return;
	}

	spa_assert(t->profile & SPA_BT_PROFILE_HEADSET_AUDIO);

	if (rfcomm->telephony_ag && rfcomm->telephony_ag->transport.rejectSCO) {
		spa_log_info(backend->log, "rejecting SCO, AudioGatewayTransport1.RejectSCO=true");
		return;
	}

	if (t->fd >= 0) {
		spa_log_debug(backend->log, "transport %p: Rejecting, audio already connected", t);
		return;
	}

	spa_log_debug(backend->log, "transport %p: codec=%s", t, t->media_codec->description);
	if (backend->defer_setup_enabled) {
		/* In BT_DEFER_SETUP mode, when a connection is accepted, the listening socket is unblocked but
		 * the effective connection setup happens only on first receive, allowing to configure the
		 * accepted socket. */
		char buff;

		if (t->media_codec->id != SPA_BLUETOOTH_AUDIO_CODEC_CVSD) {
			/* set correct socket options for mSBC/LC3 */
			struct bt_voice voice_config;
			memset(&voice_config, 0, sizeof(voice_config));
			voice_config.setting = BT_VOICE_TRANSPARENT;
			if (setsockopt(sock, SOL_BLUETOOTH, BT_VOICE, &voice_config, sizeof(voice_config)) < 0) {
				spa_log_error(backend->log, "transport %p: setsockopt(): %s", t, strerror(errno));
				return;
			}
		}

		/* First read from the accepted socket is non-blocking and returns a zero length buffer. */
		if (read(sock, &buff, 1) == -1) {
			spa_log_error(backend->log, "transport %p: Couldn't authorize SCO connection: %s", t, strerror(errno));
			return;
		}
	}

	t->fd = spa_steal_fd(sock);

	sco_start_source(t);

	spa_log_debug(backend->log, "transport %p: audio connected", t);

	/* Report initial volume to remote */
	if (t->profile == SPA_BT_PROFILE_HSP_AG) {
		rfcomm_send_volume_cmd(rfcomm, hsp_hs_vgs, NULL, SPA_BT_VOLUME_ID_RX);
	} else if (t->profile == SPA_BT_PROFILE_HFP_AG && rfcomm->hf_state > hfp_hf_vgs) {
		/* Report volume only if SLC and setup sequence has been completed
		 * else this could break the sequence.
		 * The volumes will be reported at the end of the setup sequence. */
		rfcomm_send_volume_cmd(rfcomm, hfp_hf_vgs, NULL, SPA_BT_VOLUME_ID_RX);
	}

	spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_PENDING);
}

static void sco_listen(struct impl *backend)
{
	struct sockaddr_sco addr;
	uint32_t defer = 1;

	spa_autoclose int sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, "socket(SEQPACKET, SCO) %m");
		return;
	}

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, BDADDR_ANY);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		spa_log_error(backend->log, "bind(): %m");
		return;
	}

	if (setsockopt(sock, SOL_BLUETOOTH, BT_DEFER_SETUP, &defer, sizeof(defer)) < 0) {
		spa_log_warn(backend->log, "Can't enable deferred setup: %s", strerror(errno));
		backend->defer_setup_enabled = 0;
	} else {
		backend->defer_setup_enabled = 1;
	}

	spa_log_debug(backend->log, "doing listen");
	if (listen(sock, 1) < 0) {
		spa_log_error(backend->log, "listen(): %m");
		return;
	}

	backend->sco.func = sco_listen_event;
	backend->sco.data = backend;
	backend->sco.fd = spa_steal_fd(sock);
	backend->sco.mask = SPA_IO_IN;
	backend->sco.rmask = 0;
	spa_loop_add_source(backend->main_loop, &backend->sco);

	return;
}

static int rfcomm_ag_set_volume(struct spa_bt_transport *t, int id)
{
	struct transport_data *td = t->user_data;
	struct rfcomm *rfcomm = td->rfcomm;
	const char *format;
	int value;

	if (!rfcomm_hw_volume_enabled(rfcomm)
	    || !(rfcomm->profile & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
	    || !(rfcomm->has_volume && rfcomm->volumes[id].active))
		return -ENOTSUP;

	value = rfcomm->volumes[id].hw_volume;

	if (id == SPA_BT_VOLUME_ID_RX)
		if (rfcomm->profile & SPA_BT_PROFILE_HFP_HF)
			format = "+VGM: %d";
		else
			format = "+VGM=%d";
	else if (id == SPA_BT_VOLUME_ID_TX)
		if (rfcomm->profile & SPA_BT_PROFILE_HFP_HF)
			format = "+VGS: %d";
		else
			format = "+VGS=%d";
	else
		spa_assert_not_reached();

	if (rfcomm->transport)
		rfcomm_send_reply(rfcomm, format, value);

	return 0;
}

static int sco_set_volume_cb(void *data, int id, float volume)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_transport_volume *t_volume = &t->volumes[id];
	struct transport_data *td = t->user_data;
	struct rfcomm *rfcomm = td->rfcomm;
	int value;

	if (!rfcomm_hw_volume_enabled(rfcomm)
	    || !(rfcomm->profile & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
	    || !(rfcomm->has_volume && rfcomm->volumes[id].active))
		return -ENOTSUP;

	value = spa_bt_volume_linear_to_hw(volume, t_volume->hw_volume_max);
	t_volume->volume = volume;

	if (rfcomm->volumes[id].hw_volume == value)
		return 0;
	rfcomm->volumes[id].hw_volume = value;

	return rfcomm_ag_set_volume(t, id);
}

static const struct spa_bt_transport_implementation sco_transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = sco_acquire_cb,
	.release = sco_release_cb,
	.set_volume = sco_set_volume_cb,
	.destroy = sco_destroy_cb,
};

static struct rfcomm *device_find_rfcomm(struct impl *backend, struct spa_bt_device *device,
		enum spa_bt_profile profile)
{
	struct rfcomm *rfcomm;
	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (rfcomm->device == device && (rfcomm->profile & profile))
			return rfcomm;
	}
	return NULL;
}

static int backend_native_supports_codec(void *data, struct spa_bt_device *device, unsigned int codec)
{
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	struct impl *backend = data;
	struct rfcomm *rfcomm;

	rfcomm = device_find_rfcomm(backend, device, SPA_BT_PROFILE_HFP_HF);
	if (rfcomm == NULL)
		return -ENOTSUP;

	if (codec == HFP_AUDIO_CODEC_CVSD)
		return 1;

	if (!rfcomm->codec_negotiation_supported)
		return 0;

	return codec_list_get(backend, &rfcomm->supported_codec_list, codec) ? 1 : 0;
#else
	return -ENOTSUP;
#endif
}

static int codec_switch_stop_timer(struct rfcomm *rfcomm)
{
	struct impl *backend = rfcomm->backend;
	struct itimerspec ts;

	if (rfcomm->timer.data == NULL)
		return 0;

	spa_loop_remove_source(backend->main_loop, &rfcomm->timer);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(backend->main_system, rfcomm->timer.fd, 0, &ts, NULL);
	spa_system_close(backend->main_system, rfcomm->timer.fd);
	rfcomm->timer.data = NULL;
	return 0;
}

static void volume_sync_stop_timer(struct rfcomm *rfcomm)
{
	if (rfcomm->volume_sync_timer)
		spa_loop_utils_update_timer(rfcomm->backend->loop_utils, rfcomm->volume_sync_timer,
				NULL, NULL, false);
}

static void volume_sync_timer_event(void *data, uint64_t expirations)
{
	struct rfcomm *rfcomm = data;

	volume_sync_stop_timer(rfcomm);

	if (rfcomm->transport) {
		rfcomm_ag_set_volume(rfcomm->transport, SPA_BT_VOLUME_ID_TX);
		rfcomm_ag_set_volume(rfcomm->transport, SPA_BT_VOLUME_ID_RX);
	}
}

static int volume_sync_start_timer(struct rfcomm *rfcomm)
{
	struct timespec ts;
	const uint64_t timeout = 1500 * SPA_NSEC_PER_MSEC;

	if (rfcomm->volume_sync_timer == NULL)
		rfcomm->volume_sync_timer = spa_loop_utils_add_timer(rfcomm->backend->loop_utils,
				volume_sync_timer_event, rfcomm);

	if (rfcomm->volume_sync_timer == NULL)
		return -EIO;

	ts.tv_sec = timeout / SPA_NSEC_PER_SEC;
	ts.tv_nsec = timeout % SPA_NSEC_PER_SEC;
	spa_loop_utils_update_timer(rfcomm->backend->loop_utils, rfcomm->volume_sync_timer,
			&ts, NULL, false);

	return 0;
}

static int rfcomm_ag_sync_volume(struct rfcomm *rfcomm, bool later)
{
	if (rfcomm->transport == NULL)
		return -ENOENT;

	if (!later) {
		rfcomm_ag_set_volume(rfcomm->transport, SPA_BT_VOLUME_ID_TX);
		rfcomm_ag_set_volume(rfcomm->transport, SPA_BT_VOLUME_ID_RX);
	} else {
		volume_sync_start_timer(rfcomm);
	}

	return 0;
}

static void codec_switch_timer_event(struct spa_source *source)
{
	struct rfcomm *rfcomm = source->data;
	struct impl *backend = rfcomm->backend;
	const struct media_codec *best_codec;
	uint64_t exp;

	if (spa_system_timerfd_read(backend->main_system, source->fd, &exp) < 0)
		spa_log_warn(backend->log, "error reading timerfd: %s", strerror(errno));

	codec_switch_stop_timer(rfcomm);

	spa_log_debug(backend->log, "rfcomm %p: codec switch timeout", rfcomm);

	switch (rfcomm->hfp_ag_initial_codec_setup) {
	case HFP_AG_INITIAL_CODEC_SETUP_SEND:
		/* Retry codec selection */
		best_codec = codec_list_best(backend, &rfcomm->supported_codec_list);
		if (best_codec && best_codec->id != SPA_BLUETOOTH_AUDIO_CODEC_CVSD) {
			rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_WAIT;
			rfcomm_send_reply(rfcomm, "+BCS: %u", best_codec->codec_id);
			codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_TIMEOUT_MSEC);
			return;
		}
		SPA_FALLTHROUGH;
	case HFP_AG_INITIAL_CODEC_SETUP_WAIT:
		/* Failure, try falling back to CVSD. */
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_NONE;
		if (rfcomm->transport == NULL) {
			if (rfcomm_new_transport(rfcomm, HFP_AUDIO_CODEC_CVSD) == 0) {
				spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
			}
		}
		rfcomm_send_reply(rfcomm, "+BCS: 1");
		return;
	default:
		break;
	}

	if (rfcomm->hfp_ag_switching_codec) {
		rfcomm->hfp_ag_switching_codec = false;
		if (rfcomm->device)
			spa_bt_device_emit_codec_switched(rfcomm->device, -EIO);
	}
}

static int codec_switch_start_timer(struct rfcomm *rfcomm, int timeout_msec)
{
	struct impl *backend = rfcomm->backend;
	struct itimerspec ts;

	spa_log_debug(backend->log, "rfcomm %p: start timer", rfcomm);
	if (rfcomm->timer.data == NULL) {
		rfcomm->timer.data = rfcomm;
		rfcomm->timer.func = codec_switch_timer_event;
		rfcomm->timer.fd = spa_system_timerfd_create(backend->main_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		rfcomm->timer.mask = SPA_IO_IN;
		rfcomm->timer.rmask = 0;
		spa_loop_add_source(backend->main_loop, &rfcomm->timer);
	}
	ts.it_value.tv_sec = timeout_msec / SPA_MSEC_PER_SEC;
	ts.it_value.tv_nsec = (timeout_msec % SPA_MSEC_PER_SEC) * SPA_NSEC_PER_MSEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(backend->main_system, rfcomm->timer.fd, 0, &ts, NULL);
	return 0;
}

static int backend_native_ensure_codec(void *data, struct spa_bt_device *device, unsigned int codec)
{
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	struct impl *backend = data;
	struct rfcomm *rfcomm;
	int res;

	res = backend_native_supports_codec(data, device, codec);
	if (res < 0)
		return res;
	else if (!res)
		return -EINVAL;

	rfcomm = device_find_rfcomm(backend, device, SPA_BT_PROFILE_HFP_HF);
	if (rfcomm == NULL)
		return -ENOTSUP;

	if (!rfcomm->codec_negotiation_supported)
		return -ENOTSUP;

	if (rfcomm->codec == codec) {
		spa_bt_device_emit_codec_switched(device, 0);
		return 0;
	}

	if ((res = rfcomm_send_reply(rfcomm, "+BCS: %u", codec)) < 0)
		return res;

	rfcomm->hfp_ag_switching_codec = true;
	codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_TIMEOUT_MSEC);

	return 0;
#else
	return -ENOTSUP;
#endif
}

static void device_destroy(void *data)
{
	struct rfcomm *rfcomm = data;
	rfcomm_free(rfcomm);
}

static const struct spa_bt_device_events device_events = {
	SPA_VERSION_BT_DEVICE_EVENTS,
	.destroy = device_destroy,
};

static enum spa_bt_profile path_to_profile(const char *path)
{
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (spa_streq(path, PROFILE_HSP_AG))
		return SPA_BT_PROFILE_HSP_HS;

	if (spa_streq(path, PROFILE_HSP_HS))
		return SPA_BT_PROFILE_HSP_AG;
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (spa_streq(path, PROFILE_HFP_AG))
		return SPA_BT_PROFILE_HFP_HF;

	if (spa_streq(path, PROFILE_HFP_HF))
		return SPA_BT_PROFILE_HFP_AG;
#endif

	return SPA_BT_PROFILE_NULL;
}

static DBusHandlerResult profile_new_connection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter it;
	const char *handler, *path;
	enum spa_bt_profile profile;
	struct rfcomm *rfcomm;
	struct spa_bt_device *d;
	spa_autoclose int fd = -1;

	if (!dbus_message_has_signature(m, "oha{sv}")) {
		spa_log_warn(backend->log, "invalid NewConnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	profile = path_to_profile(handler);
	if (profile == SPA_BT_PROFILE_NULL) {
		spa_log_warn(backend->log, "invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it);
	dbus_message_iter_get_basic(&it, &path);

	d = spa_bt_device_find(backend->monitor, path);
	if (d == NULL || d->adapter == NULL) {
		spa_log_warn(backend->log, "unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_bt_device_add_profile(d, profile);

	/* Prevent to connect HSP/HFP in both directions, i.e. HS->AG and AG->HS.
	 * This may only occur when connecting to a device which provides both
	 * HS and AG which should not be the case with headsets and phones. */
	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (spa_streq(rfcomm->device->address, d->address) &&
				spa_streq(rfcomm->device->adapter->address, d->adapter->address)) {
			bool connected = false;

			switch (profile) {
			case SPA_BT_PROFILE_HFP_HF:
				if (rfcomm->profile == SPA_BT_PROFILE_HFP_AG)
					connected = true;
				break;
			case SPA_BT_PROFILE_HFP_AG:
				if (rfcomm->profile == SPA_BT_PROFILE_HFP_HF)
					connected = true;
				break;
			case SPA_BT_PROFILE_HSP_HS:
				if (rfcomm->profile == SPA_BT_PROFILE_HSP_AG)
					connected = true;
				break;
			case SPA_BT_PROFILE_HSP_AG:
				if (rfcomm->profile == SPA_BT_PROFILE_HSP_HS)
					connected = true;
				break;
			default:
				spa_log_warn(backend->log, "Unsupported profile: %s", handler);
				return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
			}

			if (connected) {
				spa_log_debug(backend->log, "Already connected in the opposite direction");
				return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
			}
		}
	}

	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &fd);

	spa_log_debug(backend->log, "NewConnection path=%s, fd=%d, profile %s", path, fd, handler);

	rfcomm = calloc(1, sizeof(struct rfcomm));
	if (rfcomm == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	rfcomm->backend = backend;
	rfcomm->profile = profile;
	rfcomm->device = d;
	rfcomm->path = strdup(path);
	rfcomm->source.func = rfcomm_event;
	rfcomm->source.data = rfcomm;
	rfcomm->source.fd = spa_steal_fd(fd);
	rfcomm->source.mask = SPA_IO_IN;
	rfcomm->source.rmask = 0;
	spa_list_init(&rfcomm->cmd_send_queue);
	spa_list_init(&rfcomm->updated_call_list);
	/* By default all indicators are enabled */
	rfcomm->cind_enabled_indicators = 0xFFFFFFFF;
	memset(rfcomm->hf_indicators, 0, sizeof rfcomm->hf_indicators);

	spa_list_init(&rfcomm->available_codec_list);
	spa_list_init(&rfcomm->supported_codec_list);

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM; ++i) {
		if (rfcomm->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)
			rfcomm->volumes[i].active = true;
		rfcomm->volumes[i].hw_volume = SPA_BT_VOLUME_INVALID;
	}

	spa_bt_device_add_listener(d, &rfcomm->device_listener, &device_events, rfcomm);
	spa_loop_add_source(backend->main_loop, &rfcomm->source);
	spa_list_append(&backend->rfcomm_list, &rfcomm->link);

	if (profile == SPA_BT_PROFILE_HSP_HS || profile == SPA_BT_PROFILE_HSP_AG) {
		if (rfcomm_new_transport(rfcomm, HFP_AUDIO_CODEC_CVSD) < 0)
			goto fail_need_memory;

		rfcomm->has_volume = rfcomm_hw_volume_enabled(rfcomm);

		if (profile == SPA_BT_PROFILE_HSP_AG) {
			rfcomm->hs_state = hsp_hs_init1;
		}

		spa_bt_device_connect_profile(rfcomm->device, profile);

		spa_log_debug(backend->log, "Transport %s available for profile %s",
				rfcomm->transport->path, handler);
	} else if (profile == SPA_BT_PROFILE_HFP_AG) {
		/* Start SLC connection */
		unsigned int hf_features = SPA_BT_HFP_HF_FEATURE_CLIP | SPA_BT_HFP_HF_FEATURE_3WAY |
									SPA_BT_HFP_HF_FEATURE_ECNR |
									SPA_BT_HFP_HF_FEATURE_ENHANCED_CALL_STATUS |
									SPA_BT_HFP_HF_FEATURE_ESCO_S4;

		make_available_codec_list(backend, rfcomm->device, &rfcomm->available_codec_list);
		rfcomm->codec_negotiation_supported = false;

		/* Decide if we want to signal that the HF supports mSBC/LC3 negotiation
		   This should be done when the bluetooth adapter supports the necessary transport mode */
		if (!spa_list_is_empty(&rfcomm->available_codec_list))
			hf_features |= SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION;

		rfcomm->has_volume = true;
		hf_features |= SPA_BT_HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL;

		/* send command to AG with the features supported by Hands-Free */
		rfcomm_send_cmd(rfcomm, hfp_hf_brsf, NULL, "AT+BRSF=%u", hf_features);

	} else if (profile == SPA_BT_PROFILE_HFP_HF) {
		make_available_codec_list(backend, rfcomm->device, &rfcomm->available_codec_list);
	}

	if (rfcomm_hw_volume_enabled(rfcomm) && (profile == SPA_BT_PROFILE_HFP_HF || profile == SPA_BT_PROFILE_HSP_HS)) {
		uint32_t device_features;
		if (spa_bt_quirks_get_features(backend->quirks, d->adapter, d, &device_features) == 0) {
			rfcomm->broken_mic_hw_volume = !(device_features & SPA_BT_FEATURE_HW_VOLUME_MIC);
			if (rfcomm->broken_mic_hw_volume)
				spa_log_debug(backend->log, "microphone HW volume disabled by quirk");
		}
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		goto fail_need_memory;
	if (!dbus_connection_send(conn, r, NULL))
		goto fail_need_memory;

	return DBUS_HANDLER_RESULT_HANDLED;

fail_need_memory:
	if (rfcomm)
		rfcomm_free(rfcomm);
	return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	spa_autoptr(DBusMessage) r = NULL;
	const char *handler, *path;
	struct spa_bt_device *d;
	enum spa_bt_profile profile = SPA_BT_PROFILE_NULL;
	DBusMessageIter it[5];
	struct rfcomm *rfcomm, *rfcomm_tmp;

	if (!dbus_message_has_signature(m, "o")) {
		spa_log_warn(backend->log, "invalid RequestDisconnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	profile = path_to_profile(handler);
	if (profile == SPA_BT_PROFILE_NULL) {
		spa_log_warn(backend->log, "invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = spa_bt_device_find(backend->monitor, path);
	if (d == NULL || d->adapter == NULL) {
		spa_log_warn(backend->log, "unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	spa_list_for_each_safe(rfcomm, rfcomm_tmp, &backend->rfcomm_list, link) {
		if (rfcomm->device == d && rfcomm->profile == profile) {
			rfcomm_free(rfcomm);
		}
	}
	spa_bt_device_check_profiles(d, false);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	const char *path, *interface, *member;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(backend->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = PROFILE_INTROSPECT_XML;
		spa_autoptr(DBusMessage) r = NULL;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(backend->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "Release"))
		res = profile_release(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "RequestDisconnection"))
		res = profile_request_disconnection(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "NewConnection"))
		res = profile_new_connection(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void register_profile_reply(DBusPendingCall *pending, void *user_data)
{
	struct impl *backend = user_data;

	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(backend->log, "Register profile not supported");
		return;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(backend->log, "Error registering profile");
		return;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "RegisterProfile() failed: %s",
				dbus_message_get_error_name(r));
		return;
	}
}

static int register_profile(struct impl *backend, const char *profile, const char *uuid)
{
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter it[4];
	dbus_bool_t autoconnect;
	dbus_uint16_t version, chan, features;
	const char *str;

	if (!(backend->enabled_profiles & spa_bt_profile_from_uuid(uuid)))
		return -ECANCELED;

	spa_log_debug(backend->log, "Registering Profile %s %s", profile, uuid);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MANAGER_INTERFACE, "RegisterProfile");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &it[0]);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_OBJECT_PATH, &profile);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_open_container(&it[0], DBUS_TYPE_ARRAY, "{sv}", &it[1]);

	if (spa_streq(uuid, SPA_BT_UUID_HSP_HS) ||
	    spa_streq(uuid, SPA_BT_UUID_HSP_HS_ALT)) {

		/* In the headset role, the connection will only be initiated from the remote side */
		str = "AutoConnect";
		autoconnect = 0;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "b", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_BOOLEAN, &autoconnect);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		str = "Channel";
		chan = HSP_HS_DEFAULT_CHANNEL;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &chan);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		/* HSP version 1.2 */
		str = "Version";
		version = 0x0102;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	} else if (spa_streq(uuid, SPA_BT_UUID_HFP_AG)) {
		str = "Features";

		features = 0;

		if (spa_bt_get_hfp_codec(backend->monitor, HFP_AUDIO_CODEC_MSBC))
			features |= SPA_BT_HFP_SDP_AG_FEATURE_WIDEBAND_SPEECH;
		if (spa_bt_get_hfp_codec(backend->monitor, HFP_AUDIO_CODEC_LC3_SWB))
			features |= SPA_BT_HFP_SDP_AG_FEATURE_SUPER_WIDEBAND_SPEECH;

		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &features);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		/* HFP version 1.9 */
		str = "Version";
		version = 0x0109;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	} else if (spa_streq(uuid, SPA_BT_UUID_HFP_HF)) {
		str = "Features";

		features = 0;

		if (spa_bt_get_hfp_codec(backend->monitor, HFP_AUDIO_CODEC_MSBC))
			features |= SPA_BT_HFP_SDP_HF_FEATURE_WIDEBAND_SPEECH;
		if (spa_bt_get_hfp_codec(backend->monitor, HFP_AUDIO_CODEC_LC3_SWB))
			features |= SPA_BT_HFP_SDP_HF_FEATURE_SUPER_WIDEBAND_SPEECH;

		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &features);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		/* HFP version 1.9 */
		str = "Version";
		version = 0x0109;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	}
	dbus_message_iter_close_container(&it[0], &it[1]);

	if (!send_with_reply(backend->conn, m, register_profile_reply, backend))
		return -EIO;

	return 0;
}

static void unregister_profile(struct impl *backend, const char *profile)
{
	spa_autoptr(DBusMessage) m = NULL, r = NULL;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	spa_log_debug(backend->log, "Unregistering Profile %s", profile);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MANAGER_INTERFACE, "UnregisterProfile");
	if (m == NULL)
		return;

	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &profile, DBUS_TYPE_INVALID);

	r = dbus_connection_send_with_reply_and_block(backend->conn, m, -1, &err);
	if (r == NULL) {
		spa_log_info(backend->log, "Unregistering Profile %s failed", profile);
		return;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "UnregisterProfile() returned error: %s", dbus_message_get_error_name(r));
		return;
	}
}

static int backend_native_register_profiles(void *data)
{
	struct impl *backend = data;

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	register_profile(backend, PROFILE_HSP_AG, SPA_BT_UUID_HSP_AG);
	register_profile(backend, PROFILE_HSP_HS, SPA_BT_UUID_HSP_HS);
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	register_profile(backend, PROFILE_HFP_AG, SPA_BT_UUID_HFP_AG);
	register_profile(backend, PROFILE_HFP_HF, SPA_BT_UUID_HFP_HF);
#endif

	if (backend->enabled_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
		sco_listen(backend);

	return 0;
}

static void sco_close(struct impl *backend)
{
	if (backend->sco.fd >= 0) {
		if (backend->sco.loop)
			spa_loop_remove_source(backend->sco.loop, &backend->sco);
		shutdown(backend->sco.fd, SHUT_RDWR);
		close (backend->sco.fd);
		backend->sco.fd = -1;
	}
}

static int backend_native_unregister_profiles(void *data)
{
	struct impl *backend = data;

	sco_close(backend);

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (backend->enabled_profiles & SPA_BT_PROFILE_HSP_AG)
		unregister_profile(backend, PROFILE_HSP_AG);
	if (backend->enabled_profiles & SPA_BT_PROFILE_HSP_HS)
		unregister_profile(backend, PROFILE_HSP_HS);
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (backend->enabled_profiles & SPA_BT_PROFILE_HFP_AG)
		unregister_profile(backend, PROFILE_HFP_AG);
	if (backend->enabled_profiles & SPA_BT_PROFILE_HFP_HF)
		unregister_profile(backend, PROFILE_HFP_HF);
#endif

	return 0;
}

static void send_ciev_for_each_rfcomm(struct impl *backend, int indicator, int value)
{
	struct rfcomm *rfcomm;

	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (rfcomm->profile == SPA_BT_PROFILE_HFP_HF && rfcomm->slc_configured &&
		    ((indicator == CIND_CALL || indicator == CIND_CALLSETUP || indicator == CIND_CALLHELD) ||
			(rfcomm->cind_call_notify && (rfcomm->cind_enabled_indicators & (1 << indicator)))))
			rfcomm_send_reply(rfcomm, "+CIEV: %d,%d", indicator, value);
	}
}

static void ring_timer_event(void *data, uint64_t expirations)
{
	struct impl *backend = data;
	const char *number;
	unsigned int type;
	struct timespec ts;
	const uint64_t timeout = 1 * SPA_NSEC_PER_SEC;
	struct rfcomm *rfcomm;

	number = mm_get_incoming_call_number(backend->modemmanager);
	if (number) {
		if (spa_strstartswith(number, "+"))
			type = INTERNATIONAL_NUMBER;
		else
			type = NATIONAL_NUMBER;
	}

	ts.tv_sec = timeout / SPA_NSEC_PER_SEC;
	ts.tv_nsec = timeout % SPA_NSEC_PER_SEC;
	spa_loop_utils_update_timer(backend->loop_utils, backend->ring_timer, &ts, NULL, false);

	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (rfcomm->profile == SPA_BT_PROFILE_HFP_HF  && rfcomm->slc_configured) {
			rfcomm_send_reply(rfcomm, "RING");
			if (rfcomm->clip_notify && number)
				rfcomm_send_reply(rfcomm, "+CLIP: \"%s\",%u", number, type);
		}
	}
}

static void set_call_active(bool active, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.active_call != active) {
		backend->modem.active_call = active;
		send_ciev_for_each_rfcomm(backend, CIND_CALL, active);
	}
}

static void set_call_setup(enum call_setup value, void *user_data)
{
	struct impl *backend = user_data;
	enum call_setup old = backend->modem.call_setup;

	if (backend->modem.call_setup != value) {
		backend->modem.call_setup = value;
		send_ciev_for_each_rfcomm(backend, CIND_CALLSETUP, value);
	}

	if (value == CIND_CALLSETUP_INCOMING) {
		if (backend->ring_timer == NULL)
			backend->ring_timer = spa_loop_utils_add_timer(backend->loop_utils, ring_timer_event, backend);

		if (backend->ring_timer == NULL) {
			spa_log_warn(backend->log, "Failed to create ring timer");
			return;
		}

		ring_timer_event(backend, 0);
	} else if (old == CIND_CALLSETUP_INCOMING) {
		spa_loop_utils_update_timer(backend->loop_utils, backend->ring_timer, NULL, NULL, false);
	}
}

static void set_battery_level(unsigned int level, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->battery_level != level) {
		backend->battery_level = level;
		send_ciev_for_each_rfcomm(backend, CIND_BATTERY_LEVEL, level);
	}
}

static void set_modem_operator_name(const char *name, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.operator_name) {
		free(backend->modem.operator_name);
		backend->modem.operator_name = NULL;
	}

	if (name)
		backend->modem.operator_name = strdup(name);
}

static void set_modem_roaming(bool is_roaming, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.network_is_roaming != is_roaming) {
		backend->modem.network_is_roaming = is_roaming;
		send_ciev_for_each_rfcomm(backend, CIND_ROAM, is_roaming);
	}
}

static void set_modem_own_number(const char *number, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.own_number) {
		free(backend->modem.own_number);
		backend->modem.own_number = NULL;
	}

	if (number)
		backend->modem.own_number = strdup(number);
}

static void set_modem_service(bool available, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.network_has_service != available) {
		backend->modem.network_has_service = available;
		send_ciev_for_each_rfcomm(backend, CIND_SERVICE, available);
	}
}

static void set_modem_signal_strength(unsigned int strength, void *user_data)
{
	struct impl *backend = user_data;

	if (backend->modem.signal_strength != strength) {
		backend->modem.signal_strength = strength;
		send_ciev_for_each_rfcomm(backend, CIND_SIGNAL, strength);
	}
}

static void send_cmd_result(bool success, enum cmee_error error, void *user_data)
{
	struct rfcomm *rfcomm = user_data;

	if (success) {
		rfcomm_send_reply(rfcomm, "OK");
		return;
	}

	rfcomm_send_error(rfcomm, error);
}

static int backend_native_free(void *data)
{
	struct impl *backend = data;

	struct rfcomm *rfcomm;

	sco_close(backend);

	if (backend->modemmanager) {
		mm_unregister(backend->modemmanager);
		backend->modemmanager = NULL;
	}

	if (backend->upower) {
		upower_unregister(backend->upower);
		backend->upower = NULL;
	}

	spa_clear_ptr(backend->telephony, telephony_free);

	if (backend->ring_timer)
		spa_loop_utils_destroy_source(backend->loop_utils, backend->ring_timer);

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_AG);
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_HS);
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HFP_AG);
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HFP_HF);
#endif

	spa_list_consume(rfcomm, &backend->rfcomm_list, link)
		rfcomm_free(rfcomm);

	if (backend->modem.operator_name)
		free(backend->modem.operator_name);
	free(backend);

	return 0;
}

static int parse_headset_roles(struct impl *backend, const struct spa_dict *info)
{
	const char *str;
	int profiles = SPA_BT_PROFILE_NULL;

	if (!info)
		goto fallback;

	if ((str = spa_dict_lookup(info, PROP_KEY_ROLES)) == NULL &&
			(str = spa_dict_lookup(info, PROP_KEY_HEADSET_ROLES)) == NULL)
		goto fallback;

	profiles = spa_bt_profiles_from_json_array(str);
	if (profiles < 0)
		goto fallback;

	backend->enabled_profiles = profiles & SPA_BT_PROFILE_HEADSET_AUDIO;
	return 0;
fallback:
	backend->enabled_profiles = DEFAULT_ENABLED_PROFILES;
	return 0;
}

static void parse_hfp_disable_nrec(struct impl *backend, const struct spa_dict *info)
{
	const char *str;

	if ((str = spa_dict_lookup(info, PROP_KEY_HFP_DISABLE_NREC)) != NULL)
		backend->hfp_disable_nrec = spa_atob(str);
	else
		backend->hfp_disable_nrec = false;
}

static void parse_hfp_pts(struct impl *backend, const struct spa_dict *info)
{
	const char *str;

	if ((str = spa_dict_lookup(info, "bluez5.hfphsp-backend-native-pts")) != NULL)
		backend->pts = spa_atob(str);
	else
		backend->pts = false;
}

static void parse_hfp_default_volumes(struct impl *backend, const struct spa_dict *info)
{
	const char *str;
	int vol = -1;

	if ((str = spa_dict_lookup(info, PROP_KEY_HFP_DEFAULT_MIC_VOL)) != NULL)
		spa_atoi32(str, &vol, 10);

	if (vol >= 0 && vol <= 15)
		backend->hfp_default_mic_volume = vol;
	else
		backend->hfp_default_mic_volume = SPA_BT_VOLUME_HS_MAX;

	vol = -1;
	if ((str = spa_dict_lookup(info, PROP_KEY_HFP_DEFAULT_SPEAKER_VOL)) != NULL)
		spa_atoi32(str, &vol, 10);

	if (vol >= 0 && vol <= 15)
		backend->hfp_default_speaker_volume = vol;
	else
		backend->hfp_default_speaker_volume = SPA_BT_VOLUME_HS_MAX;
}

static void parse_sco_datapath(struct impl *backend, const struct spa_dict *info)
{
	backend->hfphsp_sco_datapath = HFP_SCO_DEFAULT_DATAPATH;

	spa_atou32(spa_dict_lookup(info, "bluez5.hw-offload-datapath"),
		&backend->hfphsp_sco_datapath, 10);
}

static const struct spa_bt_backend_implementation backend_impl = {
	SPA_VERSION_BT_BACKEND_IMPLEMENTATION,
	.free = backend_native_free,
	.register_profiles = backend_native_register_profiles,
	.unregister_profiles = backend_native_unregister_profiles,
	.ensure_codec = backend_native_ensure_codec,
	.supports_codec = backend_native_supports_codec,
};

static const struct mm_ops mm_ops = {
	.send_cmd_result = send_cmd_result,
	.set_modem_service = set_modem_service,
	.set_modem_signal_strength = set_modem_signal_strength,
	.set_modem_operator_name = set_modem_operator_name,
	.set_modem_own_number = set_modem_own_number,
	.set_modem_roaming = set_modem_roaming,
	.set_call_active = set_call_active,
	.set_call_setup = set_call_setup,
};

struct spa_bt_backend *backend_native_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *backend;

	static const DBusObjectPathVTable vtable_profile = {
		.message_function = profile_handler,
	};

	backend = calloc(1, sizeof(struct impl));
	if (backend == NULL)
		return NULL;

	spa_bt_backend_set_implementation(&backend->this, &backend_impl, backend);

	backend->this.name = "native";
	backend->monitor = monitor;
	backend->quirks = quirks;
	backend->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	backend->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	backend->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	backend->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);
	backend->loop_utils = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_LoopUtils);
	backend->conn = dbus_connection;
	backend->sco.fd = -1;

	backend->codecs = spa_bt_get_media_codecs(monitor);

	spa_log_topic_init(backend->log, &log_topic);

	spa_list_init(&backend->rfcomm_list);

	if (parse_headset_roles(backend, info) < 0)
		goto fail;

	parse_hfp_disable_nrec(backend, info);
	parse_hfp_default_volumes(backend, info);
	parse_hfp_pts(backend, info);
	parse_sco_datapath(backend, info);

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HSP_AG,
						  &vtable_profile, backend)) {
		goto fail;
	}

	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HSP_HS,
						  &vtable_profile, backend)) {
		goto fail1;
	}
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HFP_AG,
						  &vtable_profile, backend)) {
		goto fail2;
	}

	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HFP_HF,
						  &vtable_profile, backend)) {
		goto fail3;
	}
#endif

	backend->modemmanager = mm_register(backend->log, backend->conn, info, &mm_ops, backend);
	backend->upower = upower_register(backend->log, backend->conn, set_battery_level, backend);
	backend->telephony = telephony_new(backend->log, backend->dbus, info);

	return &backend->this;

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
fail3:
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HFP_AG);
fail2:
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_HS);
fail1:
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_AG);
#endif
fail:
	free(backend);
	return NULL;
}
