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
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/utils/json.h>
#include <spa/param/audio/raw.h>

#include "defs.h"

#ifdef HAVE_LIBUSB
#include <libusb.h>
#endif

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.native");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define PROP_KEY_HEADSET_ROLES "bluez5.headset-roles"

#define HFP_CODEC_SWITCH_INITIAL_TIMEOUT_MSEC 5000
#define HFP_CODEC_SWITCH_TIMEOUT_MSEC 20000

enum {
	HFP_AG_INITIAL_CODEC_SETUP_NONE = 0,
	HFP_AG_INITIAL_CODEC_SETUP_SEND,
	HFP_AG_INITIAL_CODEC_SETUP_WAIT
};

struct impl {
	struct spa_bt_backend this;

	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;
	struct spa_dbus *dbus;
	DBusConnection *conn;

#define DEFAULT_ENABLED_PROFILES (SPA_BT_PROFILE_HSP_HS | SPA_BT_PROFILE_HFP_AG)
	enum spa_bt_profile enabled_profiles;

	struct spa_source sco;

	const struct spa_bt_quirks *quirks;

	struct spa_list rfcomm_list;
	unsigned int defer_setup_enabled:1;
};

struct transport_data {
	struct rfcomm *rfcomm;
	struct spa_source sco;
};

enum hfp_hf_state {
	hfp_hf_brsf,
	hfp_hf_bac,
	hfp_hf_cind1,
	hfp_hf_cind2,
	hfp_hf_cmer,
	hfp_hf_slc1,
	hfp_hf_slc2,
	hfp_hf_vgs,
	hfp_hf_vgm,
	hfp_hf_bcs
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
	char* path;
	bool has_volume;
	struct rfcomm_volume volumes[SPA_BT_VOLUME_ID_TERM];
	unsigned int broken_mic_hw_volume:1;
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	unsigned int slc_configured:1;
	unsigned int codec_negotiation_supported:1;
	unsigned int msbc_supported_by_hfp:1;
	unsigned int hfp_ag_switching_codec:1;
	unsigned int hfp_ag_initial_codec_setup:2;
	unsigned int cind_call_active:1;
	unsigned int cind_call_notify:1;
	enum hfp_hf_state hf_state;
	enum hsp_hs_state hs_state;
	unsigned int codec;
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
	struct impl *backend = rfcomm->backend;

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
	struct impl *backend = rfcomm->backend;
	struct spa_bt_transport *t = NULL;
	struct transport_data *td;
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
	t->backend = &backend->this;
	t->n_channels = 1;
	t->channels[0] = SPA_AUDIO_CHANNEL_MONO;

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
			t->volumes[i].volume =
				spa_bt_volume_hw_to_linear(rfcomm->volumes[i].hw_volume, t->volumes[i].hw_volume_max);
	}

	spa_bt_transport_add_listener(t, &rfcomm->transport_listener, &transport_events, rfcomm);

finish:
	return t;
}

static int codec_switch_stop_timer(struct rfcomm *rfcomm);

static void rfcomm_free(struct rfcomm *rfcomm)
{
	codec_switch_stop_timer(rfcomm);
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
	free(rfcomm);
}

#define RFCOMM_MESSAGE_MAX_LENGTH 256

SPA_PRINTF_FUNC(2, 3)
static ssize_t rfcomm_send_cmd(const struct rfcomm *rfcomm, const char *format, ...)
{
	struct impl *backend = rfcomm->backend;
	char message[RFCOMM_MESSAGE_MAX_LENGTH + 1];
	ssize_t len;
	va_list args;

	va_start(args, format);
	len = vsnprintf(message, RFCOMM_MESSAGE_MAX_LENGTH + 1, format, args);
	va_end(args);

	if (len < 0)
		return -EINVAL;

	if (len > RFCOMM_MESSAGE_MAX_LENGTH)
		return -E2BIG;

	spa_log_debug(backend->log, "RFCOMM >> %s", message);

	message[len] = '\n';
	/* `message` is no longer null-terminated */

	len = write(rfcomm->source.fd, message, len + 1);
	/* we ignore any errors, it's not critical and real errors should
	 * be caught with the HANGUP and ERROR events handled above */
	if (len < 0) {
		len = -errno;
		spa_log_error(backend->log, "RFCOMM write error: %s", strerror(errno));
	}

	return len;
}

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

static bool rfcomm_volume_enabled(struct rfcomm *rfcomm)
{
	return rfcomm->device != NULL
		&& (rfcomm->device->hw_volume_profiles & rfcomm->profile);
}

static void rfcomm_emit_volume_changed(struct rfcomm *rfcomm, int id, int hw_volume)
{
	struct spa_bt_transport_volume *t_volume;

	if (!rfcomm_volume_enabled(rfcomm))
		return;

	if ((id == SPA_BT_VOLUME_ID_RX || id == SPA_BT_VOLUME_ID_TX) && hw_volume >= 0) {
		rfcomm->volumes[id].active = true;
		rfcomm->volumes[id].hw_volume = hw_volume;
	}

	spa_log_debug(rfcomm->backend->log, "volume changed %d", hw_volume);

	if (rfcomm->transport == NULL || !rfcomm->has_volume)
		return;

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM ; ++i) {
		t_volume = &rfcomm->transport->volumes[i];
		t_volume->active = rfcomm->volumes[i].active;
		t_volume->volume =
			spa_bt_volume_hw_to_linear(rfcomm->volumes[i].hw_volume, t_volume->hw_volume_max);
	}

	spa_bt_transport_emit_volume_changed(rfcomm->transport);
}

#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
static bool rfcomm_hsp_ag(struct rfcomm *rfcomm, char* buf)
{
	struct impl *backend = rfcomm->backend;
	unsigned int gain, dummy;

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
	} else if (sscanf(buf, "AT+CKPD=%d", &dummy) == 1) {
		rfcomm_send_reply(rfcomm, "OK");
	} else {
		return false;
	}

	return true;
}

static bool rfcomm_send_volume_cmd(struct rfcomm *rfcomm, int id)
{
	struct spa_bt_transport_volume *t_volume;
	const char *format;
	int hw_volume;

	if (!rfcomm_volume_enabled(rfcomm))
		return false;

	t_volume = rfcomm->transport ? &rfcomm->transport->volumes[id] : NULL;

	if (!(t_volume && t_volume->active))
		return false;

	hw_volume = spa_bt_volume_linear_to_hw(t_volume->volume, t_volume->hw_volume_max);
	rfcomm->volumes[id].hw_volume = hw_volume;

	if (id == SPA_BT_VOLUME_ID_TX)
		format = "AT+VGM";
	else if (id == SPA_BT_VOLUME_ID_RX)
		format = "AT+VGS";
	else
	 	spa_assert_not_reached();

	rfcomm_send_cmd(rfcomm, "%s=%d", format, hw_volume);

	return true;
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
	if (sscanf(buf, "\r\n+VGS=%d\r\n", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", buf);
		}
	} else if (sscanf(buf, "\r\n+VGM=%d\r\n", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", buf);
		}
	} else if (spa_strstartswith(buf, "\r\nOK\r\n")) {
		if (rfcomm->hs_state == hsp_hs_init2) {
			if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_RX))
				rfcomm->hs_state = hsp_hs_vgs;
			else
				rfcomm->hs_state = hsp_hs_init1;
		} else if (rfcomm->hs_state == hsp_hs_vgs) {
			if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_TX))
				rfcomm->hs_state = hsp_hs_vgm;
			else
				rfcomm->hs_state = hsp_hs_init1;
		}
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
		libusb_free_device_list(devices, 0);
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

static bool device_supports_required_mSBC_transport_modes(
		struct impl *backend, struct spa_bt_device *device)
{
	int res;
	bool msbc_ok, msbc_alt1_ok;
	uint32_t bt_features;

	if (device->adapter == NULL)
		return false;

	if (backend->quirks && spa_bt_quirks_get_features(backend->quirks, device->adapter, device, &bt_features) == 0) {
		msbc_ok = bt_features & SPA_BT_FEATURE_MSBC;
		msbc_alt1_ok = bt_features & (SPA_BT_FEATURE_MSBC_ALT1 | SPA_BT_FEATURE_MSBC_ALT1_RTL);
	} else {
		msbc_ok = true;
		msbc_alt1_ok = true;
	}

	spa_log_info(backend->log,
			"bluez-monitor/hardware.conf: msbc:%d msbc-alt1:%d", (int)msbc_ok, (int)msbc_alt1_ok);

	if (!msbc_ok && !msbc_alt1_ok)
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
	if (device->adapter->bus_type == BUS_TYPE_USB && !msbc_alt1_ok && msbc_ok) {
#ifdef HAVE_LIBUSB
		if (device->adapter->source_id == SOURCE_ID_USB) {
			msbc_ok = check_usb_altsetting_6(backend, device->adapter->vendor_id,
					device->adapter->product_id);
		} else {
			msbc_ok = false;
		}
		if (!msbc_ok)
			spa_log_info(backend->log, "bluetooth host adapter does not support USB ALT6");
#else
		spa_log_info(backend->log,
			"compiled without libusb; can't check if bluetooth adapter has USB ALT6");
		msbc_ok = false;
#endif
	}
	if (device->adapter->bus_type != BUS_TYPE_USB)
		msbc_alt1_ok = false;

	return msbc_ok || msbc_alt1_ok;
}

static int codec_switch_start_timer(struct rfcomm *rfcomm, int timeout_msec);

static void process_iphoneaccev_indicator(struct rfcomm *rfcomm, unsigned int key, unsigned int value)
{
	struct impl *backend = rfcomm->backend;

	spa_log_debug(backend->log, "key:%u value:%u", key, value);

	if (key == SPA_BT_HFP_HF_IPHONEACCEV_KEY_BATTERY_LEVEL) {
		// Battery level is reported in range of 0-9, convert to 10-100%
		uint8_t level = (SPA_CLAMP(value, 0u, 9u) + 1) * 10;
		spa_log_debug(backend->log, "battery level: %d%%", (int) level);

		// TODO: report without Battery Provider (using props)
		spa_bt_device_report_battery_level(rfcomm->device, level);
	} else {
		spa_log_warn(backend->log, "unknown AT+IPHONEACCEV key:%u value:%u", key, value);
	}
}

static void process_hfp_hf_indicator(struct rfcomm *rfcomm, unsigned int indicator, unsigned int value)
{
	struct impl *backend = rfcomm->backend;

	spa_log_debug(backend->log, "indicator:%u value:%u", indicator, value);

	if (indicator == SPA_BT_HFP_HF_INDICATOR_BATTERY_LEVEL) {
		// Battery level is reported in range 0-100
		spa_log_debug(backend->log, "battery level: %u%%", value);

		if (value <= 100) {
			// TODO: report without Battery Provider (using props)
			spa_bt_device_report_battery_level(rfcomm->device, value);
		} else {
			spa_log_warn(backend->log, "battery HF indicator %u outside of range [0, 100]: %u", indicator, value);
		}
	} else {
		spa_log_warn(backend->log, "unknown HF indicator:%u value:%u", indicator, value);
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

		/* Decide if we want to signal that the computer supports mSBC negotiation
		   This should be done when the computers bluetooth adapter supports the necessary transport mode */
		if (device_supports_required_mSBC_transport_modes(backend, rfcomm->device)) {

			/* set the feature bit that indicates AG (=computer) supports codec negotiation */
			ag_features |= SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION;

			/* let's see if the headset supports codec negotiation */
			if ((features & (SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION)) != 0) {
				spa_log_debug(backend->log,
					"RFCOMM features = %i, codec negotiation supported by headset",
					features);
				/* Prepare reply: Audio Gateway (=computer) supports codec negotiation */
				rfcomm->codec_negotiation_supported = true;
				rfcomm->msbc_supported_by_hfp = false;
			} else {
				/* Codec negotiation not supported */
				spa_log_debug(backend->log,
					"RFCOMM features = %i, codec negotiation NOT supported by headset",
					 features);

				rfcomm->codec_negotiation_supported = false;
				rfcomm->msbc_supported_by_hfp = false;
			}
		}

		/* send reply to HF with the features supported by Audio Gateway (=computer) */
		ag_features |= SPA_BT_HFP_AG_FEATURE_HF_INDICATORS;
		rfcomm_send_reply(rfcomm, "+BRSF: %u", ag_features);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+BAC=")) {
		/* retrieve supported codecs */
		/* response has the form AT+BAC=<codecID1>,<codecID2>,<codecIDx>
		   strategy: split the string into tokens */
		static const char separators[] = "=,";

		char* token;
		int cntr = 0;

		token = strtok (buf, separators);
		while (token != NULL)
		{
			/* skip token 0 i.e. the "AT+BAC=" part */
			if (cntr > 0) {
				int codec_id;
				sscanf (token, "%u", &codec_id);
				spa_log_debug(backend->log, "RFCOMM AT+BAC found codec %u", codec_id);
				if (codec_id == HFP_AUDIO_CODEC_MSBC) {
					rfcomm->msbc_supported_by_hfp = true;
					spa_log_debug(backend->log, "RFCOMM headset supports mSBC codec");
				}
			}
			/* get next token */
			token = strtok (NULL, separators);
			cntr++;
		}

		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CIND=?")) {
		rfcomm_send_reply(rfcomm, "+CIND:(\"service\",(0-1)),(\"call\",(0-1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2))");
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CIND?")) {
		rfcomm_send_reply(rfcomm, "+CIND: 0,%d,0,0", rfcomm->cind_call_active);
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+CMER")) {
		int mode, keyp, disp, ind;

		rfcomm->slc_configured = true;
		rfcomm_send_reply(rfcomm, "OK");

		rfcomm->cind_call_active = false;
		if (sscanf(buf, "AT+CMER= %d , %d , %d , %d", &mode, &keyp, &disp, &ind) == 4)
			rfcomm->cind_call_notify = ind ? true : false;
		else
			rfcomm->cind_call_notify = false;

		/* switch codec to mSBC by sending unsolicited +BCS message */
		if (rfcomm->codec_negotiation_supported && rfcomm->msbc_supported_by_hfp) {
			spa_log_debug(backend->log, "RFCOMM initial codec setup");
			rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_SEND;
			rfcomm_send_reply(rfcomm, "+BCS: 2");
			codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_INITIAL_TIMEOUT_MSEC);
		} else {
			rfcomm->transport = _transport_create(rfcomm);
			if (rfcomm->transport == NULL) {
				spa_log_warn(backend->log, "can't create transport: %m");
				// TODO: We should manage the missing transport
			} else {
				rfcomm->transport->codec = HFP_AUDIO_CODEC_CVSD;
				spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
				rfcomm_emit_volume_changed(rfcomm, -1, SPA_BT_VOLUME_INVALID);
			}
		}
	} else if (!rfcomm->slc_configured) {
		spa_log_warn(backend->log, "RFCOMM receive command before SLC completed: %s", buf);
		rfcomm_send_reply(rfcomm, "ERROR");
		return false;
	} else if (sscanf(buf, "AT+BCS=%u", &selected_codec) == 1) {
		/* parse BCS(=Bluetooth Codec Selection) reply */
		bool was_switching_codec = rfcomm->hfp_ag_switching_codec && (rfcomm->device != NULL);
		rfcomm->hfp_ag_switching_codec = false;
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_NONE;
		codec_switch_stop_timer(rfcomm);

		if (selected_codec != HFP_AUDIO_CODEC_CVSD && selected_codec != HFP_AUDIO_CODEC_MSBC) {
			spa_log_warn(backend->log, "unsupported codec negotiation: %d", selected_codec);
			rfcomm_send_reply(rfcomm, "ERROR");
			if (was_switching_codec)
				spa_bt_device_emit_codec_switched(rfcomm->device, -EIO);
			return true;
		}

		rfcomm->codec = selected_codec;

		spa_log_debug(backend->log, "RFCOMM selected_codec = %i", selected_codec);

		/* Recreate transport, since previous connection may now be invalid */
		if (rfcomm->transport)
			spa_bt_transport_free(rfcomm->transport);

		rfcomm->transport = _transport_create(rfcomm);
		if (rfcomm->transport == NULL) {
			spa_log_warn(backend->log, "can't create transport: %m");
			// TODO: We should manage the missing transport
			rfcomm_send_reply(rfcomm, "ERROR");
			if (was_switching_codec)
				spa_bt_device_emit_codec_switched(rfcomm->device, -ENOMEM);
			return true;
		}
		rfcomm->transport->codec = selected_codec;
		spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
		rfcomm_emit_volume_changed(rfcomm, -1, SPA_BT_VOLUME_INVALID);

		rfcomm_send_reply(rfcomm, "OK");
		if (was_switching_codec)
			spa_bt_device_emit_codec_switched(rfcomm->device, 0);
	} else if (spa_strstartswith(buf, "AT+BIA=")) {
		/* We only support 'call' indicator, which HFP 4.35.1 defines as
		   always active (assuming CMER enabled it), so we don't need to
		   parse anything here. */
		rfcomm_send_reply(rfcomm, "OK");
	} else if (sscanf(buf, "AT+VGM=%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			if (!rfcomm->broken_mic_hw_volume)
				rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", buf);
			rfcomm_send_reply(rfcomm, "ERROR");
		}
	} else if (sscanf(buf, "AT+VGS=%u", &gain) == 1) {
		if (gain <= SPA_BT_VOLUME_HS_MAX) {
			rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
			rfcomm_send_reply(rfcomm, "OK");
		} else {
			spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", buf);
			rfcomm_send_reply(rfcomm, "ERROR");
		}
	} else if (spa_strstartswith(buf, "AT+BIND=?")) {
		rfcomm_send_reply(rfcomm, "+BIND: (2)");
		rfcomm_send_reply(rfcomm, "OK");
	} else if (spa_strstartswith(buf, "AT+BIND?")) {
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
	} else if (spa_strstartswith(buf, "AT+APLSIRI?")) {
		// This command is sent when we activate Apple extensions
		rfcomm_send_reply(rfcomm, "OK");
	} else {
		return false;
	}

	return true;
}

static bool rfcomm_hfp_hf(struct rfcomm *rfcomm, char* buf)
{
	static const char separators[] = "\r\n:";

	struct impl *backend = rfcomm->backend;
	unsigned int features;
	unsigned int gain;
	unsigned int selected_codec;
	char* token;

	token = strtok(buf, separators);
	while (token != NULL)
	{
		if (spa_strstartswith(token, "+BRSF")) {
			/* get next token */
			token = strtok(NULL, separators);
			features = atoi(token);
			if (((features & (SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION)) != 0) &&
			    rfcomm->msbc_supported_by_hfp)
				rfcomm->codec_negotiation_supported = true;
		} else if (spa_strstartswith(token, "+BCS") && rfcomm->codec_negotiation_supported) {
			/* get next token */
			token = strtok(NULL, separators);
			selected_codec = atoi(token);

			if (selected_codec != HFP_AUDIO_CODEC_CVSD && selected_codec != HFP_AUDIO_CODEC_MSBC) {
				spa_log_warn(backend->log, "unsupported codec negotiation: %d", selected_codec);
			} else {
				spa_log_debug(backend->log, "RFCOMM selected_codec = %i", selected_codec);

				/* send codec selection to AG */
				rfcomm_send_cmd(rfcomm, "AT+BCS=%u", selected_codec);

				rfcomm->hf_state = hfp_hf_bcs;

				if (!rfcomm->transport || (rfcomm->transport->codec != selected_codec) ) {
					if (rfcomm->transport)
						spa_bt_transport_free(rfcomm->transport);

					rfcomm->transport = _transport_create(rfcomm);
					if (rfcomm->transport == NULL) {
						spa_log_warn(backend->log, "can't create transport: %m");
						// TODO: We should manage the missing transport
					} else {
						rfcomm->transport->codec = selected_codec;
						spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
					}
				}
			}
		} else if (spa_strstartswith(token, "+CIND")) {
			/* get next token and discard it */
			token = strtok(NULL, separators);
		} else if (spa_strstartswith(token, "+VGM")) {
			/* get next token */
			token = strtok(NULL, separators);
			gain = atoi(token);

			if (gain <= SPA_BT_VOLUME_HS_MAX) {
				rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_TX, gain);
			} else {
				spa_log_debug(backend->log, "RFCOMM receive unsupported VGM gain: %s", token);
			}
		} else if (spa_strstartswith(token, "+VGS")) {
			/* get next token */
			token = strtok(NULL, separators);
			gain = atoi(token);

			if (gain <= SPA_BT_VOLUME_HS_MAX) {
				rfcomm_emit_volume_changed(rfcomm, SPA_BT_VOLUME_ID_RX, gain);
			} else {
				spa_log_debug(backend->log, "RFCOMM receive unsupported VGS gain: %s", token);
			}
		} else if (spa_strstartswith(token, "OK")) {
			switch(rfcomm->hf_state) {
				case hfp_hf_brsf:
					if (rfcomm->codec_negotiation_supported) {
						rfcomm_send_cmd(rfcomm, "AT+BAC=1,2");
						rfcomm->hf_state = hfp_hf_bac;
					} else {
						rfcomm_send_cmd(rfcomm, "AT+CIND=?");
						rfcomm->hf_state = hfp_hf_cind1;
					}
					break;
				case hfp_hf_bac:
					rfcomm_send_cmd(rfcomm, "AT+CIND=?");
					rfcomm->hf_state = hfp_hf_cind1;
					break;
				case hfp_hf_cind1:
					rfcomm_send_cmd(rfcomm, "AT+CIND?");
					rfcomm->hf_state = hfp_hf_cind2;
					break;
				case hfp_hf_cind2:
					rfcomm_send_cmd(rfcomm, "AT+CMER=3,0,0,0");
					rfcomm->hf_state = hfp_hf_cmer;
					break;
				case hfp_hf_cmer:
					rfcomm->hf_state = hfp_hf_slc1;
					rfcomm->slc_configured = true;
					if (!rfcomm->codec_negotiation_supported) {
						rfcomm->transport = _transport_create(rfcomm);
						if (rfcomm->transport == NULL) {
							spa_log_warn(backend->log, "can't create transport: %m");
							// TODO: We should manage the missing transport
						} else {
							rfcomm->transport->codec = HFP_AUDIO_CODEC_CVSD;
							spa_bt_device_connect_profile(rfcomm->device, rfcomm->profile);
						}
					}
					/* Report volume on SLC establishment */
					if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_RX))
						rfcomm->hf_state = hfp_hf_vgs;
					break;
				case hfp_hf_slc2:
					if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_RX))
						rfcomm->hf_state = hfp_hf_vgs;
					break;
				case hfp_hf_vgs:
					rfcomm->hf_state = hfp_hf_slc1;
					if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_TX))
						rfcomm->hf_state = hfp_hf_vgm;
					break;
				default:
					break;
			}
		}
		/* get next token */
		token = strtok(NULL, separators);
	}

	return true;
}

#endif

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
		bool res = false;

		len = read(source->fd, buf, 511);
		if (len < 0) {
			spa_log_error(backend->log, "RFCOMM read error: %s", strerror(errno));
			return;
		}
		buf[len] = 0;
		spa_log_debug(backend->log, "RFCOMM << %s", buf);

		switch (rfcomm->profile) {
#ifdef HAVE_BLUEZ_5_BACKEND_HSP_NATIVE
		case SPA_BT_PROFILE_HSP_HS:
			res = rfcomm_hsp_ag(rfcomm, buf);
			break;
		case SPA_BT_PROFILE_HSP_AG:
			res = rfcomm_hsp_hs(rfcomm, buf);
			break;
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
		case SPA_BT_PROFILE_HFP_HF:
			res = rfcomm_hfp_ag(rfcomm, buf);
			break;
		case SPA_BT_PROFILE_HFP_AG:
			res = rfcomm_hfp_hf(rfcomm, buf);
			break;
#endif
		default:
			break;
		}

		if (!res) {
			spa_log_debug(backend->log, "RFCOMM received unsupported command: %s", buf);
			rfcomm_send_reply(rfcomm, "ERROR");
		}
	}
}

static int sco_create_socket(struct impl *backend, struct spa_bt_adapter *adapter, bool msbc)
{
	struct sockaddr_sco addr;
	socklen_t len;
	bdaddr_t src;
	int sock = -1;

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, "socket(SEQPACKET, SCO) %s", strerror(errno));
		goto fail;
	}

	str2ba(adapter->address, &src);

	len = sizeof(addr);
	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, len) < 0) {
		spa_log_error(backend->log, "bind(): %s", strerror(errno));
		goto fail;
	}

	spa_log_debug(backend->log, "msbc=%d", (int)msbc);
	if (msbc) {
		/* set correct socket options for mSBC */
		struct bt_voice voice_config;
		memset(&voice_config, 0, sizeof(voice_config));
		voice_config.setting = BT_VOICE_TRANSPARENT;
		if (setsockopt(sock, SOL_BLUETOOTH, BT_VOICE, &voice_config, sizeof(voice_config)) < 0) {
			spa_log_error(backend->log, "setsockopt(): %s", strerror(errno));
			goto fail;
		}
	}

	return sock;

fail:
	if (sock >= 0)
		close(sock);
	return -1;
}

static int sco_do_connect(struct spa_bt_transport *t)
{
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	struct spa_bt_device *d = t->device;
	struct transport_data *td = t->user_data;
	struct sockaddr_sco addr;
	socklen_t len;
	int err;
	int sock;
	bdaddr_t dst;
	int retry = 2;

	spa_log_debug(backend->log, "transport %p: enter sco_do_connect, codec=%u",
			t, t->codec);

	if (d->adapter == NULL)
		return -EIO;

	str2ba(d->address, &dst);

again:
	sock = sco_create_socket(backend, d->adapter, (t->codec == HFP_AUDIO_CODEC_MSBC));
	if (sock < 0)
		return -1;

	len = sizeof(addr);
	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &dst);

	spa_log_debug(backend->log, "transport %p: doing connect", t);
	err = connect(sock, (struct sockaddr *) &addr, len);
	if (err < 0 && errno == ECONNABORTED && retry-- > 0) {
		spa_log_warn(backend->log, "connect(): %s. Remaining retry:%d",
				strerror(errno), retry);
		close(sock);
		goto again;
	} else if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
		spa_log_error(backend->log, "connect(): %s", strerror(errno));
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
		if (errno == EOPNOTSUPP && t->codec == HFP_AUDIO_CODEC_MSBC &&
				td->rfcomm->msbc_supported_by_hfp) {
			/* Adapter doesn't support msbc. Renegotiate. */
			d->adapter->msbc_probed = true;
			d->adapter->has_msbc = false;
			td->rfcomm->msbc_supported_by_hfp = false;
			if (t->profile == SPA_BT_PROFILE_HFP_HF) {
				td->rfcomm->hfp_ag_switching_codec = true;
				rfcomm_send_reply(td->rfcomm, "+BCS: 1");
			} else if (t->profile == SPA_BT_PROFILE_HFP_AG) {
				rfcomm_send_cmd(td->rfcomm, "AT+BAC=1");
			}
		}
#endif
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
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);
	int sock;
	socklen_t len;

	spa_log_debug(backend->log, "transport %p: enter sco_acquire_cb", t);

	if (optional || t->fd > 0)
		sock = t->fd;
	else
		sock = sco_do_connect(t);

	if (sock < 0)
		goto fail;

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	rfcomm_hfp_ag_set_cind(td->rfcomm, true);
#endif

	t->fd = sock;

	/* Fallback value */
	t->read_mtu = 48;
	t->write_mtu = 48;

	if (true) {
		struct sco_options sco_opt;

		len = sizeof(sco_opt);
		memset(&sco_opt, 0, len);

		if (getsockopt(sock, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0)
			spa_log_warn(backend->log, "getsockopt(SCO_OPTIONS) failed, loading defaults");
		else {
			spa_log_debug(backend->log, "autodetected mtu = %u", sco_opt.mtu);
			t->read_mtu = sco_opt.mtu;
			t->write_mtu = sco_opt.mtu;
		}
	}
	spa_log_debug(backend->log, "transport %p: read_mtu=%u, write_mtu=%u", t, t->read_mtu, t->write_mtu);

	return 0;

fail:
	return -1;
}

static int sco_release_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct transport_data *td = t->user_data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);

	spa_log_info(backend->log, "Transport %s released", t->path);

#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	rfcomm_hfp_ag_set_cind(td->rfcomm, false);
#endif

	if (t->sco_io) {
		spa_bt_sco_io_destroy(t->sco_io);
		t->sco_io = NULL;
	}

	if (t->fd > 0) {
		/* Shutdown and close the socket */
		shutdown(t->fd, SHUT_RDWR);
		close(t->fd);
		t->fd = -1;
	}

	return 0;
}

static void sco_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct impl *backend = SPA_CONTAINER_OF(t->backend, struct impl, this);

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_debug(backend->log, "transport %p: error on SCO socket: %s", t, strerror(errno));
		if (t->fd >= 0) {
			if (source->loop)
				spa_loop_remove_source(source->loop, source);
			shutdown(t->fd, SHUT_RDWR);
			close (t->fd);
			t->fd = -1;
			spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_IDLE);
		}
	}
}

static void sco_listen_event(struct spa_source *source)
{
	struct impl *backend = source->data;
	struct sockaddr_sco addr;
	socklen_t addrlen;
	int sock = -1;
	char local_address[18], remote_address[18];
	struct rfcomm *rfcomm;
	struct spa_bt_transport *t = NULL;
	struct transport_data *td;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_error(backend->log, "error listening SCO connection: %s", strerror(errno));
		goto fail;
	}

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	spa_log_debug(backend->log, "doing accept");
	sock = accept(source->fd, (struct sockaddr *) &addr, &addrlen);
	if (sock < 0) {
		if (errno != EAGAIN)
			spa_log_error(backend->log, "SCO accept(): %s", strerror(errno));
		goto fail;
	}

	ba2str(&addr.sco_bdaddr, remote_address);

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if (getsockname(sock, (struct sockaddr *) &addr, &addrlen) < 0) {
		spa_log_error(backend->log, "SCO getsockname(): %s", strerror(errno));
		goto fail;
	}

	ba2str(&addr.sco_bdaddr, local_address);

	/* Find transport for local and remote address */
	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (rfcomm->transport && spa_streq(rfcomm->transport->device->address, remote_address) &&
		    spa_streq(rfcomm->transport->device->adapter->address, local_address)) {
					t = rfcomm->transport;
					break;
		}
	}
	if (!t) {
		spa_log_debug(backend->log, "No transport for adapter %s and remote %s",
		              local_address, remote_address);
		goto fail;
	}

	/* The Synchronous Connection shall always be established by the AG, i.e. the remote profile
	   should be a HSP AG or HFP AG profile */
	if ((t->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) == 0) {
		spa_log_debug(backend->log, "transport %p: Rejecting incoming audio connection to an AG profile", t);
		goto fail;
	}

	if (t->fd >= 0) {
		spa_log_debug(backend->log, "transport %p: Rejecting, audio already connected", t);
		goto fail;
	}

	spa_log_debug(backend->log, "transport %p: codec=%u", t, t->codec);
	if (backend->defer_setup_enabled) {
		/* In BT_DEFER_SETUP mode, when a connection is accepted, the listening socket is unblocked but
		 * the effective connection setup happens only on first receive, allowing to configure the
		 * accepted socket. */
		char buff;

		if (t->codec == HFP_AUDIO_CODEC_MSBC) {
			/* set correct socket options for mSBC */
			struct bt_voice voice_config;
			memset(&voice_config, 0, sizeof(voice_config));
			voice_config.setting = BT_VOICE_TRANSPARENT;
			if (setsockopt(sock, SOL_BLUETOOTH, BT_VOICE, &voice_config, sizeof(voice_config)) < 0) {
				spa_log_error(backend->log, "transport %p: setsockopt(): %s", t, strerror(errno));
				goto fail;
			}
		}

		/* First read from the accepted socket is non-blocking and returns a zero length buffer. */
		if (read(sock, &buff, 1) == -1) {
			spa_log_error(backend->log, "transport %p: Couldn't authorize SCO connection: %s", t, strerror(errno));
			goto fail;
		}
	}

	t->fd = sock;

	td = t->user_data;
	td->sco.func = sco_event;
	td->sco.data = t;
	td->sco.fd = sock;
	td->sco.mask = SPA_IO_HUP | SPA_IO_ERR;
	td->sco.rmask = 0;
	spa_loop_add_source(backend->main_loop, &td->sco);

	spa_log_debug(backend->log, "transport %p: audio connected", t);

	/* Report initial volume to remote */
	if (t->profile == SPA_BT_PROFILE_HSP_AG) {
		if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_RX))
			rfcomm->hs_state = hsp_hs_vgs;
		else
			rfcomm->hs_state = hsp_hs_init1;
	} else if (t->profile == SPA_BT_PROFILE_HFP_AG) {
		if (rfcomm_send_volume_cmd(rfcomm, SPA_BT_VOLUME_ID_RX))
			rfcomm->hf_state = hfp_hf_vgs;
		else
			rfcomm->hf_state = hfp_hf_slc1;
	}

	spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_PENDING);
	return;

fail:
	if (sock >= 0)
		close(sock);
	return;
}

static int sco_listen(struct impl *backend)
{
	struct sockaddr_sco addr;
	int sock;
	uint32_t defer = 1;

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(backend->log, "socket(SEQPACKET, SCO) %m");
		return -errno;
	}

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, BDADDR_ANY);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		spa_log_error(backend->log, "bind(): %m");
		goto fail_close;
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
		goto fail_close;
	}

	backend->sco.func = sco_listen_event;
	backend->sco.data = backend;
	backend->sco.fd = sock;
	backend->sco.mask = SPA_IO_IN;
	backend->sco.rmask = 0;
	spa_loop_add_source(backend->main_loop, &backend->sco);

	return sock;

fail_close:
	close(sock);
	return -1;
}

static int sco_set_volume_cb(void *data, int id, float volume)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_transport_volume *t_volume = &t->volumes[id];
	struct transport_data *td = t->user_data;
	struct rfcomm *rfcomm = td->rfcomm;
	const char *format;
	int value;

	if (!rfcomm_volume_enabled(rfcomm)
	    || !(rfcomm->profile & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
	    || !(rfcomm->has_volume && rfcomm->volumes[id].active))
		return -ENOTSUP;

	value = spa_bt_volume_linear_to_hw(volume, t_volume->hw_volume_max);
	t_volume->volume = volume;

	if (rfcomm->volumes[id].hw_volume == value)
		return 0;
	rfcomm->volumes[id].hw_volume = value;

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

static const struct spa_bt_transport_implementation sco_transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = sco_acquire_cb,
	.release = sco_release_cb,
	.set_volume = sco_set_volume_cb,
};

static struct rfcomm *device_find_rfcomm(struct impl *backend, struct spa_bt_device *device)
{
	struct rfcomm *rfcomm;
	spa_list_for_each(rfcomm, &backend->rfcomm_list, link) {
		if (rfcomm->device == device)
			return rfcomm;
	}
	return NULL;
}

static int backend_native_supports_codec(void *data, struct spa_bt_device *device, unsigned int codec)
{
#ifdef HAVE_BLUEZ_5_BACKEND_HFP_NATIVE
	struct impl *backend = data;
	struct rfcomm *rfcomm;

	rfcomm = device_find_rfcomm(backend, device);
	if (rfcomm == NULL || rfcomm->profile != SPA_BT_PROFILE_HFP_HF)
		return -ENOTSUP;

	if (codec == HFP_AUDIO_CODEC_CVSD)
		return 1;

	return (codec == HFP_AUDIO_CODEC_MSBC &&
	        (rfcomm->profile == SPA_BT_PROFILE_HFP_AG ||
	         rfcomm->profile == SPA_BT_PROFILE_HFP_HF) &&
	        rfcomm->msbc_supported_by_hfp &&
	        rfcomm->codec_negotiation_supported) ? 1 : 0;
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

static void codec_switch_timer_event(struct spa_source *source)
{
	struct rfcomm *rfcomm = source->data;
	struct impl *backend = rfcomm->backend;
	uint64_t exp;

	if (spa_system_timerfd_read(backend->main_system, source->fd, &exp) < 0)
		spa_log_warn(backend->log, "error reading timerfd: %s", strerror(errno));

	codec_switch_stop_timer(rfcomm);

	spa_log_debug(backend->log, "rfcomm %p: codec switch timeout", rfcomm);

	switch (rfcomm->hfp_ag_initial_codec_setup) {
	case HFP_AG_INITIAL_CODEC_SETUP_SEND:
		/* Retry codec selection */
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_WAIT;
		rfcomm_send_reply(rfcomm, "+BCS: 2");
		codec_switch_start_timer(rfcomm, HFP_CODEC_SWITCH_TIMEOUT_MSEC);
		return;
	case HFP_AG_INITIAL_CODEC_SETUP_WAIT:
		/* Failure, try falling back to CVSD. */
		rfcomm->hfp_ag_initial_codec_setup = HFP_AG_INITIAL_CODEC_SETUP_NONE;
		if (rfcomm->transport == NULL) {
			rfcomm->transport = _transport_create(rfcomm);
			if (rfcomm->transport == NULL) {
				spa_log_warn(backend->log, "can't create transport: %m");
			} else {
				rfcomm->transport->codec = HFP_AUDIO_CODEC_CVSD;
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
	if (res <= 0)
		return -EINVAL;

	rfcomm = device_find_rfcomm(backend, device);
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
	DBusMessage *r;
	DBusMessageIter it[5];
	const char *handler, *path;
	enum spa_bt_profile profile;
	struct rfcomm *rfcomm;
	struct spa_bt_device *d;
	struct spa_bt_transport *t = NULL;
	int fd;

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

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = spa_bt_device_find(backend->monitor, path);
	if (d == NULL || d->adapter == NULL) {
		spa_log_warn(backend->log, "unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_bt_device_add_profile(d, profile);

	dbus_message_iter_next(&it[0]);
	dbus_message_iter_get_basic(&it[0], &fd);

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
	rfcomm->source.fd = fd;
	rfcomm->source.mask = SPA_IO_IN;
	rfcomm->source.rmask = 0;

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM; ++i) {
		if (rfcomm->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)
			rfcomm->volumes[i].active = true;
		rfcomm->volumes[i].hw_volume = SPA_BT_VOLUME_INVALID;
	}

	spa_bt_device_add_listener(d, &rfcomm->device_listener, &device_events, rfcomm);
	spa_loop_add_source(backend->main_loop, &rfcomm->source);
	spa_list_append(&backend->rfcomm_list, &rfcomm->link);

	if (profile == SPA_BT_PROFILE_HSP_HS || profile == SPA_BT_PROFILE_HSP_AG) {
		t = _transport_create(rfcomm);
		if (t == NULL) {
			spa_log_warn(backend->log, "can't create transport: %m");
			goto fail_need_memory;
		}
		rfcomm->transport = t;
		rfcomm->has_volume = rfcomm_volume_enabled(rfcomm);

		if (profile == SPA_BT_PROFILE_HSP_AG) {
			rfcomm->hs_state = hsp_hs_init1;
		}

		spa_bt_device_connect_profile(t->device, profile);

		spa_log_debug(backend->log, "Transport %s available for profile %s", t->path, handler);
	} else if (profile == SPA_BT_PROFILE_HFP_AG) {
		/* Start SLC connection */
		unsigned int hf_features = SPA_BT_HFP_HF_FEATURE_NONE;

		/* Decide if we want to signal that the HF supports mSBC negotiation
		   This should be done when the bluetooth adapter supports the necessary transport mode */
		if (device_supports_required_mSBC_transport_modes(backend, rfcomm->device)) {
			/* set the feature bit that indicates HF supports codec negotiation */
			hf_features |= SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION;
			rfcomm->msbc_supported_by_hfp = true;
			rfcomm->codec_negotiation_supported = false;
		} else {
			rfcomm->msbc_supported_by_hfp = false;
			rfcomm->codec_negotiation_supported = false;
		}

		if (rfcomm_volume_enabled(rfcomm)) {
			rfcomm->has_volume = true;
			hf_features |= SPA_BT_HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL;
		}

		/* send command to AG with the features supported by Hands-Free */
		rfcomm_send_cmd(rfcomm, "AT+BRSF=%u", hf_features);

		rfcomm->hf_state = hfp_hf_brsf;
	}

	if (rfcomm_volume_enabled(rfcomm) && (profile == SPA_BT_PROFILE_HFP_HF || profile == SPA_BT_PROFILE_HSP_HS)) {
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
	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;

fail_need_memory:
	if (rfcomm)
		rfcomm_free(rfcomm);
	return DBUS_HANDLER_RESULT_NEED_MEMORY;
}

static DBusHandlerResult profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	DBusMessage *r;
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

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	const char *path, *interface, *member;
	DBusMessage *r;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(backend->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

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
	struct impl *backend = user_data;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(backend->log, "Register profile not supported");
		goto finish;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(backend->log, "Error registering profile");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "RegisterProfile() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
}

static int register_profile(struct impl *backend, const char *profile, const char *uuid)
{
	DBusMessage *m;
	DBusMessageIter it[4];
	dbus_bool_t autoconnect;
	dbus_uint16_t version, chan, features;
	char *str;
	DBusPendingCall *call;

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

		/* We announce wideband speech support anyway */
		features = SPA_BT_HFP_SDP_AG_FEATURE_WIDEBAND_SPEECH;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &features);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		/* HFP version 1.7 */
		str = "Version";
		version = 0x0107;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	} else if (spa_streq(uuid, SPA_BT_UUID_HFP_HF)) {
		str = "Features";

		/* We announce wideband speech support anyway */
		features = SPA_BT_HFP_SDP_HF_FEATURE_WIDEBAND_SPEECH;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &features);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

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

static void unregister_profile(struct impl *backend, const char *profile)
{
	DBusMessage *m, *r;
	DBusError err;

	spa_log_debug(backend->log, "Unregistering Profile %s", profile);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MANAGER_INTERFACE, "UnregisterProfile");
	if (m == NULL)
		return;

	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &profile, DBUS_TYPE_INVALID);

	dbus_error_init(&err);

	r = dbus_connection_send_with_reply_and_block(backend->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r == NULL) {
		spa_log_info(backend->log, "Unregistering Profile %s failed", profile);
		dbus_error_free(&err);
		return;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "UnregisterProfile() returned error: %s", dbus_message_get_error_name(r));
		return;
	}

	dbus_message_unref(r);
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

static int backend_native_free(void *data)
{
	struct impl *backend = data;

	struct rfcomm *rfcomm;

	sco_close(backend);

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

	free(backend);

	return 0;
}

static int parse_headset_roles(struct impl *backend, const struct spa_dict *info)
{
	const char *str;
	int profiles = SPA_BT_PROFILE_NULL;

	if (info == NULL ||
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

static const struct spa_bt_backend_implementation backend_impl = {
	SPA_VERSION_BT_BACKEND_IMPLEMENTATION,
	.free = backend_native_free,
	.register_profiles = backend_native_register_profiles,
	.unregister_profiles = backend_native_unregister_profiles,
	.ensure_codec = backend_native_ensure_codec,
	.supports_codec = backend_native_supports_codec,
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
	backend->conn = dbus_connection;
	backend->sco.fd = -1;

	spa_log_topic_init(backend->log, &log_topic);

	spa_list_init(&backend->rfcomm_list);

	if (parse_headset_roles(backend, info) < 0)
		goto fail;

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
