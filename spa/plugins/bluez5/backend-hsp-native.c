/* Spa HSP native backend
 *
 * Copyright © 2018 Wim Taymans
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

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/type.h>

#include "defs.h"

#define NAME "hsp-native"

struct spa_bt_backend {
	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_dbus *dbus;
	DBusConnection *conn;
};

struct transport_data {
	struct spa_source rfcomm;
	struct spa_source sco;
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

static void rfcomm_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct spa_bt_backend *backend = t->backend;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_info(backend->log, NAME": lost RFCOMM connection.");
		if (source->loop)
			spa_loop_remove_source(source->loop, source);
		goto fail;
	}

	if (source->rmask & SPA_IO_IN) {
		char buf[512];
		ssize_t len;
		int gain, dummy;
		bool  do_reply = false;

		len = read(source->fd, buf, 511);
		if (len < 0) {
			spa_log_error(backend->log, NAME": RFCOMM read error: %s", strerror(errno));
			goto fail;
		}
		buf[len] = 0;
		spa_log_debug(backend->log, NAME": RFCOMM << %s", buf);

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
//			t->speaker_gain = gain;
			do_reply = true;
		} else if (sscanf(buf, "AT+VGM=%d", &gain) == 1 ||
		    sscanf(buf, "\r\n+VGS=%d\r\n", &gain) == 1) {
//			t->microphone_gain = gain;
			do_reply = true;
		} else if (sscanf(buf, "AT+CKPD=%d", &dummy) == 1) {
			do_reply = true;
		} else {
			do_reply = false;
		}

		if (do_reply) {
			spa_log_debug(backend->log, NAME": RFCOMM >> OK");

			len = write(source->fd, "\r\nOK\r\n", 6);

			/* we ignore any errors, it's not critical and real errors should
			 * be caught with the HANGUP and ERROR events handled above */
			if (len < 0)
				spa_log_error(backend->log, NAME": RFCOMM write error: %s", strerror(errno));
		}
	}
	return;

fail:
	spa_bt_transport_free(t);
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

	return 0;

fail:
	return -1;
}

static int sco_release_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_backend *backend = t->backend;

	spa_log_info(backend->log, "Transport %s released", t->path);

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
	if (td->rfcomm.data) {
		if (td->rfcomm.loop)
			spa_loop_remove_source(td->rfcomm.loop, &td->rfcomm);
		shutdown(td->rfcomm.fd, SHUT_RDWR);
		close (td->rfcomm.fd);
		td->rfcomm.fd = -1;
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
	char *pathfd;
	enum spa_bt_profile profile;
	struct spa_bt_device *d;
	struct spa_bt_transport *t;
	struct transport_data *td;
	int fd;

	if (!dbus_message_has_signature(m, "oha{sv}")) {
		spa_log_warn(backend->log, NAME": invalid NewConnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
	else {
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

	if ((pathfd = spa_aprintf("%s/fd%d", path, fd)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	t = spa_bt_transport_create(backend->monitor, pathfd, sizeof(struct transport_data));
	if (t == NULL) {
		spa_log_warn(backend->log, NAME": can't create transport: %m");
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	spa_bt_transport_set_implementation(t, &sco_transport_impl, t);

	t->device = d;
	spa_list_append(&t->device->transport_list, &t->device_link);
	t->profile = profile;
	t->backend = backend;

	td = t->user_data;
	td->rfcomm.func = rfcomm_event;
	td->rfcomm.data = t;
	td->rfcomm.fd = fd;
	td->rfcomm.mask = SPA_IO_IN;
	td->rfcomm.rmask = 0;
	spa_loop_add_source(backend->main_loop, &td->rfcomm);

	spa_bt_device_connect_profile(t->device, profile);

	sco_listen(t);

	spa_log_debug(backend->log, NAME": Transport %s available for profile %s", t->path, handler);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	DBusMessage *r;
	const char *handler, *path;
	struct spa_bt_device *d;
	struct spa_bt_transport *t, *tmp;
	enum spa_bt_profile profile;
	DBusMessageIter it[5];

	if (!dbus_message_has_signature(m, "o")) {
		spa_log_warn(backend->log, NAME": invalid RequestDisconnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
	else {
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

	spa_list_for_each_safe(t, tmp, &d->transport_list, device_link) {
		if (t->profile == profile)
			spa_bt_transport_free(t);
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
	}
	dbus_message_iter_close_container(&it[0], &it[1]);

	dbus_connection_send_with_reply(backend->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, register_profile_reply, backend, NULL);
        dbus_message_unref(m);
	return 0;
}

void backend_hsp_native_register_profiles(struct spa_bt_backend *backend)
{
	register_profile(backend, PROFILE_HSP_AG, SPA_BT_UUID_HSP_AG);
	register_profile(backend, PROFILE_HSP_HS, SPA_BT_UUID_HSP_HS);
}

void backend_hsp_native_free(struct spa_bt_backend *backend)
{
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_AG);
	dbus_connection_unregister_object_path(backend->conn, PROFILE_HSP_HS);
	free(backend);
}

struct spa_bt_backend *backend_hsp_native_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_backend *backend;
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

	return backend;
fail:
	free(backend);
	return NULL;
}
