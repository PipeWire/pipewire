/* Spa HSP/HFP native backend
 *
 * Copyright © 2018 Wim Taymans
 * Copyright © 2021 Collabora
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

#include <errno.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/type.h>

#include "defs.h"

#define NAME "native"

struct spa_bt_backend {
	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_dbus *dbus;
	DBusConnection *conn;

	struct spa_list rfcomm_list;
	unsigned int msbc_support_enabled_in_config:1;
};

struct transport_data {
	struct spa_source sco;
};

struct rfcomm {
	struct spa_list link;
	struct spa_source source;
	struct spa_bt_backend *backend;
	struct spa_bt_device *device;
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;
	enum spa_bt_profile profile;
	char* path;
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	unsigned int slc_configured:1;
	unsigned int codec_negotiation_supported:1;
	unsigned int msbc_supported_by_hfp:1;
#endif
};

static DBusHandlerResult profile_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	DBusMessage *r;

	r = dbus_message_new_error(m, BLUEZ_PROFILE_INTERFACE ".Error.NotImplemented",
                                            "Method not implemented");
	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void transport_destroy(void *data)
{
	struct rfcomm *rfcomm = data;
	struct spa_bt_backend *backend = rfcomm->backend;

	spa_log_debug(backend->log, "transport %p destroy", rfcomm->transport);
	rfcomm->transport = NULL;
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
	.destroy = transport_destroy,
};

static const struct spa_bt_transport_implementation sco_transport_impl;

static struct spa_bt_transport *_transport_create(struct rfcomm *rfcomm)
{
	struct spa_bt_backend *backend = rfcomm->backend;
	struct spa_bt_transport *t = NULL;
	char* pathfd;

	if ((pathfd = spa_aprintf("%s/fd%d", rfcomm->path, rfcomm->source.fd)) == NULL)
		return NULL;

	t = spa_bt_transport_create(backend->monitor, pathfd, sizeof(struct transport_data));
	if (t == NULL)
		goto finish;
	spa_bt_transport_set_implementation(t, &sco_transport_impl, t);

	t->device = rfcomm->device;
	spa_list_append(&t->device->transport_list, &t->device_link);
	t->profile = rfcomm->profile;
	t->backend = backend;

	spa_bt_transport_add_listener(t, &rfcomm->transport_listener, &transport_events, rfcomm);

finish:
	return t;
}

static void rfcomm_free(struct rfcomm *rfcomm)
{
	spa_list_remove(&rfcomm->link);
	if (rfcomm->path)
		free(rfcomm->path);
	if (rfcomm->transport) {
		spa_hook_remove(&rfcomm->transport_listener);
		spa_bt_transport_free(rfcomm->transport);
	}
	free(rfcomm);
}

static void rfcomm_send_reply(struct spa_source *source, char *data)
{
	struct rfcomm *rfcomm = source->data;
	struct spa_bt_backend *backend = rfcomm->backend;
	char message[256];
	ssize_t len;

	spa_log_debug(backend->log, NAME": RFCOMM >> %s", data);
	sprintf(message, "\r\n%s\r\n", data);
	len = write(source->fd, message, strlen(message));
	/* we ignore any errors, it's not critical and real errors should
	 * be caught with the HANGUP and ERROR events handled above */
	if (len < 0)
		spa_log_error(backend->log, NAME": RFCOMM write error: %s", strerror(errno));
}

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
static bool rfcomm_hsp(struct spa_source *source, char* buf)
{
	unsigned int gain, dummy;

	/* There are only four HSP AT commands:
		* AT+VGS=value: value between 0 and 15, sent by the HS to AG to set the speaker gain.
		* +VGS=value is sent by AG to HS as a response to an AT+VGS command or when the gain
		* is changed on the AG side.
		* AT+VGM=value: value between 0 and 15, sent by the HS to AG to set the microphone gain.
		* +VGM=value is sent by AG to HS as a response to an AT+VGM command or when the gain
		* is changed on the AG side.
		* AT+CKPD=200: Sent by HS when headset button is pressed.
		* RING: Sent by AG to HS to notify of an incoming call. It can safely be ignored because
		* it does not expect a reply. */
	if (sscanf(buf, "AT+VGS=%d", &gain) == 1 ||
			sscanf(buf, "\r\n+VGM=%d\r\n", &gain) == 1) {
		/* t->speaker_gain = gain; */
		rfcomm_send_reply(source, "OK");
	} else if (sscanf(buf, "AT+VGM=%d", &gain) == 1 ||
			sscanf(buf, "\r\n+VGS=%d\r\n", &gain) == 1) {
		/* t->microphone_gain = gain; */
		rfcomm_send_reply(source, "OK");
	} else if (sscanf(buf, "AT+CKPD=%d", &dummy) == 1) {
		rfcomm_send_reply(source, "OK");
	} else {
		return false;
	}

	return true;
}
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
static bool device_supports_required_mSBC_transport_modes(
		struct spa_bt_backend *backend, struct spa_bt_device *device) {

	const char *src_addr;
	bdaddr_t src;
	int i;
	uint8_t features[8], max_page = 0;
	int device_id;
	int sock;

	if (device->adapter == NULL)
		return false;

	spa_log_debug(backend->log, NAME": Entering function");

	src_addr = device->adapter->address;

	/* don't use ba2str to avoid -lbluetooth */
	for (i = 5; i >= 0; i--, src_addr += 3)
		src.b[i] = strtol(src_addr, NULL, 16);

	device_id = hci_get_route(&src);
	sock = hci_open_dev(device_id);
		if (sock < 0) {
		spa_log_error(backend->log, NAME": Error opening device hci%d: %s (%d)\n",
						device_id, strerror(errno), errno);
		return false;
	}

	if (hci_read_local_ext_features(sock, 0, &max_page, features, 1000) < 0) {
		spa_log_error(backend->log, NAME": Error reading extended features hci%d: %s (%d)\n",
						device_id, strerror(errno), errno);
		hci_close_dev(sock);
		return false;
	}
	hci_close_dev(sock);

	if (!(features[2] & LMP_TRSP_SCO)) {
		/* When adapater support, then the LMP_TRSP_SCO bit in features[2] is set*/
		spa_log_info(backend->log,
			NAME": bluetooth host adapter not capable of Transparent SCO LMP_TRSP_SCO" );
		return false;

	} else if (!(features[3] & LMP_ESCO)) {
	  /* When adapater support, then the LMP_ESCO bit in features[3] is set*/
		spa_log_info(backend->log,
			NAME": bluetooth host adapter not capable of eSCO link mode (LMP_ESCO)" );
		return false;

	} else {
		spa_log_info(backend->log,
				NAME": bluetooth host adapter supports eSCO link and Transparent Data mode" );
		return true;
	}

	spa_log_debug(backend->log, NAME": Fallthrough - we should not be here");
	return false;
}

static bool rfcomm_hfp_ag(struct spa_source *source, char* buf)
{
	struct rfcomm *rfcomm = source->data;
	struct spa_bt_backend *backend = rfcomm->backend;
	unsigned int features;
	unsigned int gain;
	unsigned int selected_codec;

	if (sscanf(buf, "AT+BRSF=%u", &features) == 1) {

		unsigned int ag_features = SPA_BT_HFP_AG_FEATURE_NONE;
		char *cmd;

		/* Decide if we want to signal that the computer supports mSBC negotiation
		   This should be done when
			 a) mSBC support is enabled in config file and
			 b) the computers bluetooth adapter supports the necessary transport mode */
		if ((backend->msbc_support_enabled_in_config == true) &&
			  (device_supports_required_mSBC_transport_modes(backend, rfcomm->device))) {

			/* set the feature bit that indicates AG (=computer) supports codec negotiation */
			ag_features |= SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION;

			/* let's see if the headset supports codec negotiation */
			if ((features & (SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION)) != 0) {
				spa_log_debug(backend->log,
					NAME": RFCOMM features = %i, codec negotiation supported by headset",
					features);
				/* Prepare reply: Audio Gateway (=computer) supports codec negotiation */
				rfcomm->codec_negotiation_supported = true;
				rfcomm->msbc_supported_by_hfp = false;
			} else {
				/* Codec negotiation not supported */
				spa_log_debug(backend->log,
					NAME": RFCOMM features = %i, codec negotiation NOT supported by headset",
					 features);

				rfcomm->codec_negotiation_supported = false;
				rfcomm->msbc_supported_by_hfp = false;
			}
		}

		/* send reply to HF with the features supported by Audio Gateway (=computer) */
		cmd = spa_aprintf("+BRSF: %d", ag_features);
		rfcomm_send_reply(source, cmd);
		free(cmd);
		rfcomm_send_reply(source, "OK");
	} else if (strncmp(buf, "AT+BAC=", 7) == 0) {
		/* retrieve supported codecs */
		/* response has the form AT+BAC=<codecID1>,<codecID2>,<codecIDx>
		   strategy: split the string into tokens */
		char* token;
		char seperators[] = "=,";
		int cntr = 0;
		token = strtok (buf, seperators);
		while (token != NULL)
		{
			/* skip token 0 i.e. the "AT+BAC=" part */
			if (cntr > 0) {
				int codec_id;
				sscanf (token, "%u", &codec_id);
				spa_log_debug(backend->log, NAME": RFCOMM AT+BAC found codec %u", codec_id);
				if (codec_id == HFP_AUDIO_CODEC_MSBC) {
					rfcomm->msbc_supported_by_hfp = true;
					spa_log_debug(backend->log, NAME": RFCOMM headset supports mSBC codec");
				}
			}
			/* get next token */
			token = strtok (NULL, seperators);
			cntr++;
		}

		rfcomm_send_reply(source, "OK");
	} else if (strncmp(buf, "AT+CIND=?", 9) == 0) {
		rfcomm_send_reply(source, "+CIND:(\"service\",(0-1)),(\"call\",(0-1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2))");
		rfcomm_send_reply(source, "OK");
	} else if (strncmp(buf, "AT+CIND?", 8) == 0) {
		rfcomm_send_reply(source, "+CIND: 0,0,0,0");
		rfcomm_send_reply(source, "OK");
	} else if (strncmp(buf, "AT+CMER", 7) == 0) {
		rfcomm->slc_configured = true;
		rfcomm_send_reply(source, "OK");

		/* switch codec to mSBC by sending unsolicited +BCS message */
		if (rfcomm->codec_negotiation_supported && rfcomm->msbc_supported_by_hfp) {
			spa_log_debug(backend->log, NAME": RFCOMM switching codec to mSBC");
			rfcomm_send_reply(source, "+BCS: 2");
		} else {
			rfcomm->transport = _transport_create(rfcomm);
			if (rfcomm->transport == NULL) {
				spa_log_warn(backend->log, NAME": can't create transport: %m");
				// TODO: We should manage the missing transport
			}
			spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
		}

	} else if (!rfcomm->slc_configured) {
		spa_log_warn(backend->log, NAME": RFCOMM receive command before SLC completed: %s", buf);
		rfcomm_send_reply(source, "ERROR");
		return false;
	} else if (sscanf(buf, "AT+BCS=%u", &selected_codec) == 1) {
		/* parse BCS(=Bluetooth Codec Selection) reply */
		if (selected_codec != HFP_AUDIO_CODEC_CVSD && selected_codec != HFP_AUDIO_CODEC_MSBC) {
			spa_log_warn(backend->log, NAME": unsupported codec negociation: %d", selected_codec);
			rfcomm_send_reply(source, "ERROR");
			return true;
		}

		spa_log_debug(backend->log, NAME": RFCOMM selected_codec = %i", selected_codec);
		if (!rfcomm->transport || (rfcomm->transport->codec != selected_codec) ) {
			if (rfcomm->transport)
				spa_bt_transport_free(rfcomm->transport);

			rfcomm->transport = _transport_create(rfcomm);
			if (rfcomm->transport == NULL) {
				spa_log_warn(backend->log, NAME": can't create transport: %m");
				// TODO: We should manage the missing transport
			}
			rfcomm->transport->codec = selected_codec;
			spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
		}
		rfcomm_send_reply(source, "OK");
	} else if (sscanf(buf, "AT+VGM=%u", &gain) == 1) {
		/* t->microphone_gain = gain; */
		rfcomm_send_reply(source, "OK");
	} else if (sscanf(buf, "AT+VGS=%u", &gain) == 1) {
		/* t->speaker_gain = gain; */
		rfcomm_send_reply(source, "OK");
	} else {
		return false;
	}

	return true;
}
#endif

static void rfcomm_event(struct spa_source *source)
{
	struct rfcomm *rfcomm = source->data;
	struct spa_bt_backend *backend = rfcomm->backend;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_info(backend->log, NAME": lost RFCOMM connection.");
		if (source->loop)
			spa_loop_remove_source(source->loop, source);
		rfcomm_free(rfcomm);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		char buf[512];
		ssize_t len;
		bool res = false;

		len = read(source->fd, buf, 511);
		if (len < 0) {
			spa_log_error(backend->log, NAME": RFCOMM read error: %s", strerror(errno));
			return;
		}
		buf[len] = 0;
		spa_log_debug(backend->log, NAME": RFCOMM << %s", buf);

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
		if ((rfcomm->profile == SPA_BT_PROFILE_HSP_AG) || (rfcomm->profile == SPA_BT_PROFILE_HSP_HS))
			res = rfcomm_hsp(source, buf);
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
		if (rfcomm->profile == SPA_BT_PROFILE_HFP_HF)
			res = rfcomm_hfp_ag(source, buf);
#endif

		if (!res) {
			spa_log_debug(backend->log, NAME": RFCOMM receive unsupported command: %s", buf);
			rfcomm_send_reply(source, "ERROR");
		}
	}
	return;
}

static int sco_do_accept(struct spa_bt_transport *t)
{
	struct transport_data *td = t->user_data;
	struct spa_bt_backend *backend = t->backend;
	struct sockaddr_sco addr;
	socklen_t optlen;
	int sock;

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);

	spa_log_debug(backend->log, NAME": transport %p: doing accept", t);
	sock = accept(td->sco.fd, (struct sockaddr *) &addr, &optlen);
	if (sock < 0) {
		if (errno != EAGAIN)
			spa_log_error(backend->log, NAME": accept(): %s", strerror(errno));
		goto fail;
	}
	return sock;
fail:
	return -1;
}

static int sco_do_connect(struct spa_bt_transport *t)
{
	struct spa_bt_backend *backend = t->backend;
	struct spa_bt_device *d = t->device;
	struct sockaddr_sco addr;
	socklen_t len;
	int err, i;
	int sock;
	bdaddr_t src;
	bdaddr_t dst;
	const char *src_addr, *dst_addr;

	spa_log_debug(backend->log, NAME": transport %p: enter sco_do_connect", t);

	if (d->adapter == NULL)
		return -EIO;

	src_addr = d->adapter->address;
	dst_addr = d->address;

	/* don't use ba2str to avoid -lbluetooth */
	for (i = 5; i >= 0; i--, src_addr += 3)
		src.b[i] = strtol(src_addr, NULL, 16);
	for (i = 5; i >= 0; i--, dst_addr += 3)
		dst.b[i] = strtol(dst_addr, NULL, 16);

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, NAME": socket(SEQPACKET, SCO) %s", strerror(errno));
		return -errno;
	}

	len = sizeof(addr);
	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, len) < 0) {
		spa_log_error(backend->log, NAME": bind(): %s", strerror(errno));
		goto fail_close;
	}

	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &dst);

	spa_log_debug(backend->log, NAME": transport %p: codec=%u", t, t->codec);
	if (t->codec == HFP_AUDIO_CODEC_MSBC) {
		/* set correct socket options for mSBC */
		struct bt_voice voice_config;
		memset(&voice_config, 0, sizeof(voice_config));
		voice_config.setting = BT_VOICE_TRANSPARENT;
		if (setsockopt(sock, SOL_BLUETOOTH, BT_VOICE, &voice_config, sizeof(voice_config)) < 0) {
			spa_log_error(backend->log, NAME": setsockopt(): %s", strerror(errno));
			goto fail_close;
		}
	}

	spa_log_debug(backend->log, NAME": transport %p: doing connect", t);
	err = connect(sock, (struct sockaddr *) &addr, len);
	if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
		spa_log_error(backend->log, NAME": connect(): %s", strerror(errno));
		goto fail_close;
	}

	return sock;

fail_close:
	close(sock);
	return -1;
}

static int sco_acquire_cb(void *data, bool optional)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_backend *backend = t->backend;
	int sock;
	socklen_t len;

	spa_log_debug(backend->log, NAME": transport %p: enter sco_acquire_cb", t);

	if (optional)
		sock = sco_do_accept(t);
	else
		sock = sco_do_connect(t);

	if (sock < 0)
		goto fail;

	t->fd = sock;

	/* Fallback value */
	t->read_mtu = 48;
	t->write_mtu = 48;

	if (true) {
		struct sco_options sco_opt;

		len = sizeof(sco_opt);
		memset(&sco_opt, 0, len);

		if (getsockopt(sock, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0)
			spa_log_warn(backend->log, NAME": getsockopt(SCO_OPTIONS) failed, loading defaults");
		else {
			spa_log_debug(backend->log, NAME": autodetected mtu = %u", sco_opt.mtu);
			t->read_mtu = sco_opt.mtu;
			t->write_mtu = sco_opt.mtu;
		}
	}
	spa_log_debug(backend->log, NAME": transport %p: read_mtu=%u, write_mtu=%u", t, t->read_mtu, t->write_mtu);

	return 0;

fail:
	return -1;
}

static int sco_release_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_backend *backend = t->backend;

	spa_log_info(backend->log, NAME": Transport %s released", t->path);

	if (t->sco_io) {
		spa_bt_sco_io_destroy(t->sco_io);
		t->sco_io = NULL;
	}

	/* Shutdown and close the socket */
	shutdown(t->fd, SHUT_RDWR);
	close(t->fd);
	t->fd = -1;

	return 0;
}

static void sco_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct spa_bt_backend *backend = t->backend;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_error(backend->log, NAME": error listening SCO connection: %s", strerror(errno));
		goto fail;
	}

#if 0
	if (t->state != PA_BLUETOOTH_TRANSPORT_STATE_PLAYING) {
		spa_log_info(monitor->log, NAME": SCO incoming connection: changing state to PLAYING");
		pa_bluetooth_transport_set_state (t, PA_BLUETOOTH_TRANSPORT_STATE_PLAYING);
	}
#endif

fail:
	return;
}

static int sco_listen(struct spa_bt_transport *t)
{
	struct spa_bt_backend *backend = t->backend;
	struct transport_data *td = t->user_data;
	struct sockaddr_sco addr;
	int sock, i;
	bdaddr_t src;
	const char *src_addr;

	if (t->device->adapter == NULL)
		return -EIO;

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, NAME": socket(SEQPACKET, SCO) %m");
		return -errno;
	}

	src_addr = t->device->adapter->address;

	/* don't use ba2str to avoid -lbluetooth */
	for (i = 5; i >= 0; i--, src_addr += 3)
		src.b[i] = strtol(src_addr, NULL, 16);

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		spa_log_error(backend->log, NAME": bind(): %m");
		goto fail_close;
	}

	spa_log_debug(backend->log, NAME": transport %p: doing listen", t);
	if (listen(sock, 1) < 0) {
		spa_log_error(backend->log, NAME": listen(): %m");
		goto fail_close;
	}

	td->sco.func = sco_event;
	td->sco.data = t;
	td->sco.fd = sock;
	td->sco.mask = SPA_IO_IN;
	td->sco.rmask = 0;
	spa_loop_add_source(backend->main_loop, &td->sco);

	return sock;

fail_close:
	close(sock);
	return -1;
}

static int sco_destroy_cb(void *data)
{
	struct spa_bt_transport *trans = data;
	struct transport_data *td = trans->user_data;

	if (td->sco.data) {
		if (td->sco.loop)
			spa_loop_remove_source(td->sco.loop, &td->sco);
		shutdown(td->sco.fd, SHUT_RDWR);
		close (td->sco.fd);
		td->sco.fd = -1;
	}
	return 0;
}

static const struct spa_bt_transport_implementation sco_transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = sco_acquire_cb,
	.release = sco_release_cb,
	.destroy = sco_destroy_cb,
};

static DBusHandlerResult profile_new_connection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	DBusMessage *r;
	DBusMessageIter it[5];
	const char *handler, *path;
	enum spa_bt_profile profile = SPA_BT_PROFILE_NULL;
	struct rfcomm *rfcomm;
	struct spa_bt_device *d;
	struct spa_bt_transport *t = NULL;
	int fd;

	if (!dbus_message_has_signature(m, "oha{sv}")) {
		spa_log_warn(backend->log, NAME": invalid NewConnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (strcmp(handler, PROFILE_HFP_AG) == 0)
		profile = SPA_BT_PROFILE_HFP_HF;
#endif

	if (profile == SPA_BT_PROFILE_NULL) {
		spa_log_warn(backend->log, NAME": invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = spa_bt_device_find(backend->monitor, path);
	if (d == NULL) {
		spa_log_warn(backend->log, NAME": unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_next(&it[0]);
	dbus_message_iter_get_basic(&it[0], &fd);

	spa_log_debug(backend->log, NAME": NewConnection path=%s, fd=%d, profile %s", path, fd, handler);

	rfcomm = calloc(1, sizeof(struct rfcomm));
	if (rfcomm == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	rfcomm->backend = backend;
	rfcomm->profile = profile;
	rfcomm->device = d;
	rfcomm->path = strdup(path);
	rfcomm->source.func = rfcomm_event;
	rfcomm->source.data = rfcomm;
	rfcomm->source.fd = fd;
	rfcomm->source.mask = SPA_IO_IN;
	rfcomm->source.rmask = 0;

	if (profile == SPA_BT_PROFILE_HSP_HS || profile == SPA_BT_PROFILE_HSP_AG) {
		t = _transport_create(rfcomm);
		if (t == NULL) {
			spa_log_warn(backend->log, NAME": can't create transport: %m");
			goto fail_need_memory;
		}
		rfcomm->transport = t;

		spa_bt_device_connect_profile(t->device, profile);

		sco_listen(t);

		spa_log_debug(backend->log, NAME": Transport %s available for profile %s", t->path, handler);
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		goto fail_need_memory;
	if (!dbus_connection_send(conn, r, NULL))
		goto fail_need_memory;
	dbus_message_unref(r);

	spa_loop_add_source(backend->main_loop, &rfcomm->source);
	spa_list_append(&backend->rfcomm_list, &rfcomm->link);

	return DBUS_HANDLER_RESULT_HANDLED;

fail_need_memory:
	if (rfcomm)
		rfcomm_free(rfcomm);
	return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	DBusMessage *r;
	const char *handler, *path;
	struct spa_bt_device *d;
	enum spa_bt_profile profile = SPA_BT_PROFILE_NULL;
	DBusMessageIter it[5];
	struct rfcomm *rfcomm, *rfcomm_tmp;

	if (!dbus_message_has_signature(m, "o")) {
		spa_log_warn(backend->log, NAME": invalid RequestDisconnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (strcmp(handler, PROFILE_HFP_AG) == 0)
		profile = SPA_BT_PROFILE_HFP_HF;
#endif

	if (profile == SPA_BT_PROFILE_NULL) {
		spa_log_warn(backend->log, NAME": invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = spa_bt_device_find(backend->monitor, path);
	if (d == NULL) {
		spa_log_warn(backend->log, NAME": unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	spa_list_for_each_safe(rfcomm, rfcomm_tmp, &backend->rfcomm_list, link) {
		if (rfcomm->device == d && rfcomm->profile == profile) {
			if (rfcomm->source.loop)
				spa_loop_remove_source(rfcomm->source.loop, &rfcomm->source);
			shutdown(rfcomm->source.fd, SHUT_RDWR);
			close (rfcomm->source.fd);
			rfcomm->source.fd = -1;
			rfcomm_free(rfcomm);
		}
	}
	spa_bt_device_check_profiles(d, false);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	const char *path, *interface, *member;
	DBusMessage *r;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(backend->log, NAME": dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = PROFILE_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(backend->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
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
	struct spa_bt_backend *backend = user_data;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(backend->log, NAME": Register profile not supported");
		goto finish;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(backend->log, NAME": Error registering profile");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, NAME": RegisterProfile() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
}

static int register_profile(struct spa_bt_backend *backend, const char *profile, const char *uuid)
{
	DBusMessage *m;
	DBusMessageIter it[4];
	dbus_bool_t autoconnect;
	dbus_uint16_t version, chan;
	char *str;
	DBusPendingCall *call;

	spa_log_debug(backend->log, NAME": Registering Profile %s %s", profile, uuid);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MANAGER_INTERFACE, "RegisterProfile");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &it[0]);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_OBJECT_PATH, &profile);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_open_container(&it[0], DBUS_TYPE_ARRAY, "{sv}", &it[1]);

	if (strcmp(uuid, SPA_BT_UUID_HSP_HS) == 0 ||
	    strcmp(uuid, SPA_BT_UUID_HSP_HS_ALT) == 0) {

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
	} else if (strcmp(uuid, SPA_BT_UUID_HFP_AG) == 0) {
		/* HFP version 1.7 */
		str = "Version";
		version = 0x0107;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	}
	dbus_message_iter_close_container(&it[0], &it[1]);

	dbus_connection_send_with_reply(backend->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, register_profile_reply, backend, NULL);
	dbus_message_unref(m);
	return 0;
}

void backend_native_register_profiles(struct spa_bt_backend *backend)
{
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	register_profile(backend, PROFILE_HSP_AG, SPA_BT_UUID_HSP_AG);
	register_profile(backend, PROFILE_HSP_HS, SPA_BT_UUID_HSP_HS);
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	register_profile(backend, PROFILE_HFP_AG, SPA_BT_UUID_HFP_AG);
#endif
}

void backend_native_free(struct spa_bt_backend *backend)
{
	struct rfcomm *rfcomm;

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_AG);
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_HS);
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HFP_AG);
#endif

	spa_list_consume(rfcomm, &backend->rfcomm_list, link)
		rfcomm_free(rfcomm);

	free(backend);
}

struct spa_bt_backend *backend_native_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_backend *backend;
	const char *str;

	static const DBusObjectPathVTable vtable_profile = {
		.message_function = profile_handler,
	};

	backend = calloc(1, sizeof(struct spa_bt_backend));
	if (backend == NULL)
		return NULL;

	backend->monitor = monitor;
	backend->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	backend->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	backend->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	backend->conn = dbus_connection;

	spa_list_init(&backend->rfcomm_list);

	if (info && (str = spa_dict_lookup(info, "bluez5.msbc-support")))
		backend->msbc_support_enabled_in_config = strcmp(str, "true") == 0 || atoi(str) == 1;
	else
		backend->msbc_support_enabled_in_config = false;

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HSP_AG,
						  &vtable_profile, backend)) {
		goto fail;
	}

	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HSP_HS,
						  &vtable_profile, backend)) {
		goto fail;
	}
#endif

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	if (!dbus_connection_register_object_path(backend->conn,
						  PROFILE_HFP_AG,
						  &vtable_profile, backend)) {
		goto fail;
	}
#endif

	return backend;
fail:
	free(backend);
	return NULL;
}
