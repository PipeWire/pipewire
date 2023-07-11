/* Spa oFono backend */
/* SPDX-FileCopyrightText: Copyright © 2020 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/utils/result.h>
#include <spa/param/audio/raw.h>

#include "defs.h"
#include "dbus-helpers.h"

#define INITIAL_INTERVAL_NSEC	(500 * SPA_NSEC_PER_MSEC)
#define ACTION_INTERVAL_NSEC	(3000 * SPA_NSEC_PER_MSEC)

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.ofono");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct impl {
	struct spa_bt_backend this;

	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;
	struct spa_dbus *dbus;
	struct spa_loop_utils *loop_utils;
	DBusConnection *conn;

	const struct spa_bt_quirks *quirks;

	struct spa_source *timer;

	unsigned int filters_added:1;
	unsigned int msbc_supported:1;
};

struct transport_data {
	struct spa_source sco;
	unsigned int broken:1;
	unsigned int activated:1;
};

#define OFONO_HF_AUDIO_MANAGER_INTERFACE OFONO_SERVICE ".HandsfreeAudioManager"
#define OFONO_HF_AUDIO_CARD_INTERFACE OFONO_SERVICE ".HandsfreeAudioCard"
#define OFONO_HF_AUDIO_AGENT_INTERFACE OFONO_SERVICE ".HandsfreeAudioAgent"

#define OFONO_AUDIO_CLIENT	"/Profile/ofono"

#define OFONO_INTROSPECT_XML						    \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
	"<node>"                                                            \
	" <interface name=\"" OFONO_HF_AUDIO_AGENT_INTERFACE "\">"                 \
	"  <method name=\"Release\">"                                       \
	"  </method>"                                                       \
	"  <method name=\"NewConnection\">"                                 \
	"   <arg name=\"card\" direction=\"in\" type=\"o\"/>"             \
	"   <arg name=\"fd\" direction=\"in\" type=\"h\"/>"                 \
	"   <arg name=\"codec\" direction=\"in\" type=\"b\"/>"           \
	"  </method>"                                                       \
	" </interface>"                                                     \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
	"  <method name=\"Introspect\">"                                    \
	"   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
	"  </method>"                                                       \
	" </interface>"                                                     \
	"</node>"

#define OFONO_ERROR_INVALID_ARGUMENTS "org.ofono.Error.InvalidArguments"
#define OFONO_ERROR_NOT_IMPLEMENTED "org.ofono.Error.NotImplemented"
#define OFONO_ERROR_IN_USE "org.ofono.Error.InUse"
#define OFONO_ERROR_FAILED "org.ofono.Error.Failed"

static void ofono_transport_get_mtu(struct impl *backend, struct spa_bt_transport *t)
{
	struct sco_options sco_opt;
	socklen_t len;

	/* Fallback values */
	t->read_mtu = 48;
	t->write_mtu = 48;

	len = sizeof(sco_opt);
	memset(&sco_opt, 0, len);

	if (getsockopt(t->fd, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0)
		spa_log_warn(backend->log, "getsockopt(SCO_OPTIONS) failed, loading defaults");
	else {
		spa_log_debug(backend->log, "autodetected mtu = %u", sco_opt.mtu);
		t->read_mtu = sco_opt.mtu;
		t->write_mtu = sco_opt.mtu;
	}
}

static struct spa_bt_transport *_transport_create(struct impl *backend,
                                                  const char *path,
                                                  struct spa_bt_device *device,
                                                  enum spa_bt_profile profile,
                                                  int codec,
                                                  struct spa_callbacks *impl)
{
	struct spa_bt_transport *t = NULL;
	char *t_path = strdup(path);

	t = spa_bt_transport_create(backend->monitor, t_path, sizeof(struct transport_data));
	if (t == NULL) {
		spa_log_warn(backend->log, "can't create transport: %m");
		free(t_path);
		goto finish;
	}
	spa_bt_transport_set_implementation(t, impl, t);

	t->device = device;
	spa_list_append(&t->device->transport_list, &t->device_link);
	t->backend = &backend->this;
	t->profile = profile;
	t->codec = codec;
	t->n_channels = 1;
	t->channels[0] = SPA_AUDIO_CHANNEL_MONO;

finish:
	return t;
}

static int _audio_acquire(struct impl *backend, const char *path, uint8_t *codec)
{
	spa_autoptr(DBusMessage) m = NULL, r = NULL;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	int ret = 0;

	m = dbus_message_new_method_call(OFONO_SERVICE, path,
	                                 OFONO_HF_AUDIO_CARD_INTERFACE,
	                                 "Acquire");
	if (m == NULL)
		return -ENOMEM;


	/*
	 * XXX: We assume here oFono replies. It however can happen that the headset does
	 * XXX: not properly respond to the codec negotiation RFCOMM commands.
	 * XXX: oFono (1.34) fails to handle this condition, and will not send DBus reply
	 * XXX: in this case. The transport acquire API is synchronous, so we can't
	 * XXX: do better here right now.
	 */
	r = dbus_connection_send_with_reply_and_block(backend->conn, m, -1, &err);
	if (r == NULL) {
		spa_log_error(backend->log, "Transport Acquire() failed for transport %s (%s)",
		              path, err.message);
		return -EIO;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "Acquire returned error: %s", dbus_message_get_error_name(r));
		return -EIO;
	}

	if (!dbus_message_get_args(r, &err,
	                           DBUS_TYPE_UNIX_FD, &ret,
	                           DBUS_TYPE_BYTE, codec,
	                           DBUS_TYPE_INVALID)) {
		spa_log_error(backend->log, "Failed to parse Acquire() reply: %s", err.message);
		return -EIO;
	}

	return ret;
}

static int ofono_audio_acquire(void *data, bool optional)
{
	struct spa_bt_transport *transport = data;
	struct transport_data *td = transport->user_data;
	struct impl *backend = SPA_CONTAINER_OF(transport->backend, struct impl, this);
	uint8_t codec;
	int ret = 0;

	if (transport->fd >= 0)
		goto finish;
	if (td->broken) {
		ret = -EIO;
		goto finish;
	}

	spa_bt_device_update_last_bluez_action_time(transport->device);

	ret = _audio_acquire(backend, transport->path, &codec);
	if (ret < 0)
		goto finish;

	transport->fd = ret;

	if (transport->codec != codec) {
		struct timespec ts;

		spa_log_info(backend->log, "transport %p: acquired codec (%d) differs from transport one (%d)",
				transport, codec, transport->codec);

		/* shutdown to make sure connection is dropped immediately */
		shutdown(transport->fd, SHUT_RDWR);
		close(transport->fd);
		transport->fd = -1;

		/* schedule immediate profile update, from main loop */
		transport->codec = codec;
		td->broken = true;
		ts.tv_sec = 0;
		ts.tv_nsec = 1;
		spa_loop_utils_update_timer(backend->loop_utils, backend->timer,
				&ts, NULL, false);

		ret = -EIO;
		goto finish;
	}

	td->broken = false;

	spa_log_debug(backend->log, "transport %p: Acquire %s, fd %d codec %d", transport,
			transport->path, transport->fd, transport->codec);

	ofono_transport_get_mtu(backend, transport);
	ret = 0;

finish:
	if (ret < 0)
		spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_ERROR);
	else
		spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_ACTIVE);

	return ret;
}

static int ofono_audio_release(void *data)
{
	struct spa_bt_transport *transport = data;
	struct impl *backend = SPA_CONTAINER_OF(transport->backend, struct impl, this);

	spa_log_debug(backend->log, "transport %p: Release %s",
			transport, transport->path);

	spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_IDLE);

	if (transport->sco_io) {
		spa_bt_sco_io_destroy(transport->sco_io);
		transport->sco_io = NULL;
	}

	/* shutdown to make sure connection is dropped immediately */
	shutdown(transport->fd, SHUT_RDWR);
	close(transport->fd);
	transport->fd = -1;

	return 0;
}

static DBusHandlerResult ofono_audio_card_removed(struct impl *backend, const char *path)
{
	struct spa_bt_transport *transport;

	spa_assert(backend);
	spa_assert(path);

	spa_log_debug(backend->log, "card removed: %s", path);

	transport = spa_bt_transport_find(backend->monitor, path);

	if (transport != NULL) {
		struct spa_bt_device *device = transport->device;

		spa_log_debug(backend->log, "transport %p: free %s",
			transport, transport->path);

		spa_bt_transport_free(transport);
		if (device != NULL)
			spa_bt_device_check_profiles(device, false);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static const struct spa_bt_transport_implementation ofono_transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = ofono_audio_acquire,
	.release = ofono_audio_release,
};

static bool activate_transport(struct spa_bt_transport *t, const void *data)
{
	struct impl *backend = (void *)data;
	struct transport_data *td = t->user_data;
	struct timespec ts;
	uint64_t now, threshold;

	if (t->backend != &backend->this)
		return false;

	/* Check device-specific rate limit */
	spa_system_clock_gettime(backend->main_system, CLOCK_MONOTONIC, &ts);
	now = SPA_TIMESPEC_TO_NSEC(&ts);
	threshold = t->device->last_bluez_action_time + ACTION_INTERVAL_NSEC;
	if (now < threshold) {
		ts.tv_sec = (threshold - now) / SPA_NSEC_PER_SEC;
		ts.tv_nsec = (threshold - now) % SPA_NSEC_PER_SEC;
		spa_loop_utils_update_timer(backend->loop_utils, backend->timer,
				&ts, NULL, false);
		return false;
	}

	if (!td->activated) {
		/* Connect profile */
		spa_log_debug(backend->log, "Transport %s activated", t->path);
		td->activated = true;
		spa_bt_device_connect_profile(t->device, t->profile);
	}

	if (td->broken) {
		/* Recreate the transport */
		struct spa_bt_transport *t_copy;

		t_copy = _transport_create(backend, t->path, t->device,
				t->profile, t->codec, (struct spa_callbacks *)&ofono_transport_impl);
		spa_bt_transport_free(t);

		if (t_copy)
			spa_bt_device_connect_profile(t_copy->device, t_copy->profile);

		return true;
	}

	return false;
}

static void activate_transports(struct impl *backend)
{
	while (spa_bt_transport_find_full(backend->monitor, activate_transport, backend));
}

static void activate_timer_event(void *userdata, uint64_t expirations)
{
	struct impl *backend = userdata;
	spa_loop_utils_update_timer(backend->loop_utils, backend->timer, NULL, NULL, false);
	activate_transports(backend);
}

static DBusHandlerResult ofono_audio_card_found(struct impl *backend, char *path, DBusMessageIter *props_i)
{
	const char *remote_address = NULL;
	const char *local_address = NULL;
	struct spa_bt_device *d;
	struct spa_bt_transport *t;
	struct transport_data *td;
	enum spa_bt_profile profile = SPA_BT_PROFILE_HFP_AG;
	uint8_t codec = backend->msbc_supported ?
		HFP_AUDIO_CODEC_MSBC : HFP_AUDIO_CODEC_CVSD;

	spa_assert(backend);
	spa_assert(path);
	spa_assert(props_i);

	spa_log_debug(backend->log, "new card: %s", path);

	while (dbus_message_iter_get_arg_type(props_i) != DBUS_TYPE_INVALID) {
		DBusMessageIter i, value_i;
		const char *key, *value;
		char c;

		dbus_message_iter_recurse(props_i, &i);

		dbus_message_iter_get_basic(&i, &key);
		dbus_message_iter_next(&i);
		dbus_message_iter_recurse(&i, &value_i);

		if ((c = dbus_message_iter_get_arg_type(&value_i)) != DBUS_TYPE_STRING) {
			spa_log_error(backend->log, "Invalid properties for %s: expected 's', received '%c'", path, c);
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}

		dbus_message_iter_get_basic(&value_i, &value);

		if (spa_streq(key, "RemoteAddress")) {
			remote_address = value;
		} else if (spa_streq(key, "LocalAddress")) {
			local_address = value;
		} else if (spa_streq(key, "Type")) {
			if (spa_streq(value, "gateway"))
				profile = SPA_BT_PROFILE_HFP_HF;
		}

		spa_log_debug(backend->log, "%s: %s", key, value);

		dbus_message_iter_next(props_i);
	}

	if (!remote_address || !local_address) {
		spa_log_error(backend->log, "Missing addresses for %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	d = spa_bt_device_find_by_address(backend->monitor, remote_address, local_address);
	if (!d || !d->adapter) {
		spa_log_error(backend->log, "Device doesn’t exist for %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_bt_device_add_profile(d, profile);

	t = _transport_create(backend, path, d, profile, codec, (struct spa_callbacks *)&ofono_transport_impl);
	if (t == NULL) {
		spa_log_error(backend->log, "failed to create transport: %s", spa_strerror(-errno));
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	td = t->user_data;

	/*
	 * For HF profile, delay profile connect, so that we likely don't do it at the
	 * same time as the device is busy with A2DP connect. This avoids some oFono
	 * misbehavior (see comment in _audio_acquire above).
	 *
	 * For AG mode, we delay the emission of the nodes, so it is not necessary
	 * to know the codec in advance.
	 */
	if (profile == SPA_BT_PROFILE_HFP_HF) {
		struct timespec ts;
		ts.tv_sec = INITIAL_INTERVAL_NSEC / SPA_NSEC_PER_SEC;
		ts.tv_nsec = INITIAL_INTERVAL_NSEC % SPA_NSEC_PER_SEC;
		spa_loop_utils_update_timer(backend->loop_utils, backend->timer,
				&ts, NULL, false);
	} else {
		td->activated = true;
		spa_bt_device_connect_profile(t->device, t->profile);
	}

	spa_log_debug(backend->log, "Transport %s available, codec %d", t->path, t->codec);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;

	spa_log_warn(backend->log, "release");

	if (!reply_with_error(conn, m, OFONO_HF_AUDIO_AGENT_INTERFACE ".Error.NotImplemented", "Method not implemented"))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
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

static int enable_sco_socket(int sock)
{
	char c;
	struct pollfd pfd;

	if (sock < 0)
		return ENOTCONN;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = sock;
	pfd.events = POLLOUT;

	if (poll(&pfd, 1, 0) < 0)
		return errno;

	/*
	 * If socket already writable then it is not in defer setup state,
	 * otherwise it needs to be read to authorize the connection.
	 */
	if ((pfd.revents & POLLOUT))
		return 0;

	/* Enable socket by reading 1 byte */
	if (read(sock, &c, 1) < 0)
		return errno;

	return 0;
}

static DBusHandlerResult ofono_new_audio_connection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	const char *path;
	int fd;
	uint8_t codec;
	struct spa_bt_transport *t;
	struct transport_data *td;
	spa_autoptr(DBusMessage) r = NULL;

	if (dbus_message_get_args(m, NULL,
	                          DBUS_TYPE_OBJECT_PATH, &path,
	                          DBUS_TYPE_UNIX_FD, &fd,
	                          DBUS_TYPE_BYTE, &codec,
	                          DBUS_TYPE_INVALID) == FALSE) {
		r = dbus_message_new_error(m, OFONO_ERROR_INVALID_ARGUMENTS, "Invalid arguments in method call");
		goto fail;
	}

	t = spa_bt_transport_find(backend->monitor, path);
	if (t && (t->profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)) {
		int err;

		err = enable_sco_socket(fd);
		if (err) {
			spa_log_error(backend->log, "transport %p: Couldn't authorize SCO connection: %s", t, strerror(err));
			r = dbus_message_new_error(m, OFONO_ERROR_FAILED, "SCO authorization failed");
			shutdown(fd, SHUT_RDWR);
			close(fd);
			goto fail;
		}

		t->fd = fd;
		t->codec = codec;

		spa_log_debug(backend->log, "transport %p: NewConnection %s, fd %d codec %d",
						t, t->path, t->fd, t->codec);

		td = t->user_data;
		td->sco.func = sco_event;
		td->sco.data = t;
		td->sco.fd = fd;
		td->sco.mask = SPA_IO_HUP | SPA_IO_ERR;
		td->sco.rmask = 0;
		spa_loop_add_source(backend->main_loop, &td->sco);

		ofono_transport_get_mtu(backend, t);
		spa_bt_transport_set_state (t, SPA_BT_TRANSPORT_STATE_PENDING);
	}
	else if (fd) {
		spa_log_debug(backend->log, "ignoring NewConnection");
		r = dbus_message_new_error(m, OFONO_ERROR_NOT_IMPLEMENTED, "Method not implemented");
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}

fail:
	if (r) {
		if (!dbus_connection_send(backend->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct impl *backend = userdata;
	const char *path, *interface, *member;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(backend->log, "path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = OFONO_INTROSPECT_XML;
		spa_autoptr(DBusMessage) r = NULL;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(backend->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, OFONO_HF_AUDIO_AGENT_INTERFACE, "Release"))
		res = ofono_release(c, m, userdata);
	else if (dbus_message_is_method_call(m, OFONO_HF_AUDIO_AGENT_INTERFACE, "NewConnection"))
		res = ofono_new_audio_connection(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void ofono_getcards_reply(DBusPendingCall *pending, void *user_data)
{
	struct impl *backend = user_data;
	DBusMessageIter i, array_i, struct_i, props_i;

	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&pending);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "Failed to get a list of handsfree audio cards: %s",
				dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &i) || !spa_streq(dbus_message_get_signature(r), "a(oa{sv})")) {
		spa_log_error(backend->log, "Invalid arguments in GetCards() reply");
		return;
	}

	dbus_message_iter_recurse(&i, &array_i);
	while (dbus_message_iter_get_arg_type(&array_i) != DBUS_TYPE_INVALID) {
			char *path;

			dbus_message_iter_recurse(&array_i, &struct_i);
			dbus_message_iter_get_basic(&struct_i, &path);
			dbus_message_iter_next(&struct_i);

			dbus_message_iter_recurse(&struct_i, &props_i);

			ofono_audio_card_found(backend, path, &props_i);

			dbus_message_iter_next(&array_i);
	}
}

static int ofono_register(struct impl *backend)
{
	spa_autoptr(DBusMessage) m = NULL, r = NULL;
	const char *path = OFONO_AUDIO_CLIENT;
	uint8_t codecs[2];
	const uint8_t *pcodecs = codecs;
	int ncodecs = 0;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	spa_log_debug(backend->log, "Registering");

	m = dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_HF_AUDIO_MANAGER_INTERFACE, "Register");
	if (m == NULL)
		return -ENOMEM;

	codecs[ncodecs++] = HFP_AUDIO_CODEC_CVSD;
	if (backend->msbc_supported)
		codecs[ncodecs++] = HFP_AUDIO_CODEC_MSBC;

	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path,
                                          DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pcodecs, ncodecs,
                                          DBUS_TYPE_INVALID);

	r = dbus_connection_send_with_reply_and_block(backend->conn, m, -1, &err);
	if (r == NULL) {
		if (dbus_error_has_name(&err, "org.freedesktop.DBus.Error.ServiceUnknown")) {
			spa_log_info(backend->log, "oFono not available: %s",
					err.message);
			return -ENOTSUP;
		} else {
			spa_log_warn(backend->log, "Registering Profile %s failed: %s (%s)",
					path, err.message, err.name);
			return -EIO;
		}
	}

	if (dbus_message_is_error(r, OFONO_ERROR_INVALID_ARGUMENTS)) {
		spa_log_warn(backend->log, "invalid arguments");
		return -EIO;
	}
	if (dbus_message_is_error(r, OFONO_ERROR_IN_USE)) {
		spa_log_warn(backend->log, "already in use");
		return -EIO;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(backend->log, "Error registering profile");
		return -EIO;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_SERVICE_UNKNOWN)) {
		spa_log_info(backend->log, "oFono not available, disabling");
		return -EIO;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "Register() failed: %s",
				dbus_message_get_error_name(r));
		return -EIO;
	}

	spa_log_debug(backend->log, "registered");

	return 0;
}

static int ofono_getcards(struct impl *backend)
{
	spa_autoptr(DBusMessage) m = NULL;

	m = dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_HF_AUDIO_MANAGER_INTERFACE, "GetCards");
	if (m == NULL)
		return -ENOMEM;

	if (!send_with_reply(backend->conn, m, ofono_getcards_reply, backend))
		return -EIO;

	return 0;
}

static int backend_ofono_register(void *data)
{
	int ret = ofono_register(data);
	if (ret < 0)
		return ret;

	ret = ofono_getcards(data);
	if (ret < 0)
		return ret;

	return 0;
}

static DBusHandlerResult ofono_filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct impl *backend = user_data;

	if (dbus_message_is_signal(m, OFONO_HF_AUDIO_MANAGER_INTERFACE, "CardAdded")) {
			char *p;
			DBusMessageIter arg_i, props_i;

			if (!dbus_message_iter_init(m, &arg_i) || !spa_streq(dbus_message_get_signature(m), "oa{sv}")) {
					spa_log_error(backend->log, "Failed to parse org.ofono.HandsfreeAudioManager.CardAdded");
					goto fail;
			}

			dbus_message_iter_get_basic(&arg_i, &p);

			dbus_message_iter_next(&arg_i);
			spa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);

			dbus_message_iter_recurse(&arg_i, &props_i);

			return ofono_audio_card_found(backend, p, &props_i);
	} else if (dbus_message_is_signal(m, OFONO_HF_AUDIO_MANAGER_INTERFACE, "CardRemoved")) {
			const char *p;
			spa_auto(DBusError) err = DBUS_ERROR_INIT;

			if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID)) {
					spa_log_error(backend->log, "Failed to parse org.ofono.HandsfreeAudioManager.CardRemoved: %s", err.message);
					goto fail;
			}

			return ofono_audio_card_removed(backend, p);
	}

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_filters(struct impl *backend)
{
	if (backend->filters_added)
		return 0;

	if (!dbus_connection_add_filter(backend->conn, ofono_filter_cb, backend, NULL)) {
		spa_log_error(backend->log, "failed to add filter function");
		return -EIO;
	}

	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	dbus_bus_add_match(backend->conn,
			"type='signal',sender='" OFONO_SERVICE "',"
			"interface='" OFONO_HF_AUDIO_MANAGER_INTERFACE "',member='CardAdded'", &err);
	dbus_bus_add_match(backend->conn,
			"type='signal',sender='" OFONO_SERVICE "',"
			"interface='" OFONO_HF_AUDIO_MANAGER_INTERFACE "',member='CardRemoved'", &err);

	backend->filters_added = true;

	return 0;
}

static int backend_ofono_free(void *data)
{
	struct impl *backend = data;

	if (backend->filters_added) {
		dbus_connection_remove_filter(backend->conn, ofono_filter_cb, backend);
		backend->filters_added = false;
	}

	if (backend->timer)
		spa_loop_utils_destroy_source(backend->loop_utils, backend->timer);

	dbus_connection_unregister_object_path(backend->conn, OFONO_AUDIO_CLIENT);

	free(backend);

	return 0;
}

static const struct spa_bt_backend_implementation backend_impl = {
	SPA_VERSION_BT_BACKEND_IMPLEMENTATION,
	.free = backend_ofono_free,
	.register_profiles = backend_ofono_register,
};

static bool is_available(struct impl *backend)
{
	spa_autoptr(DBusMessage) m = NULL, r = NULL;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	m = dbus_message_new_method_call(OFONO_SERVICE, "/",
			DBUS_INTERFACE_INTROSPECTABLE, "Introspect");
	if (m == NULL)
		return false;

	r = dbus_connection_send_with_reply_and_block(backend->conn, m, -1, &err);
	if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_METHOD_RETURN)
		return true;

	return false;
}

struct spa_bt_backend *backend_ofono_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *backend;
	const char *str;
	static const DBusObjectPathVTable vtable_profile = {
		.message_function = ofono_handler,
	};

	backend = calloc(1, sizeof(struct impl));
	if (backend == NULL)
		return NULL;

	spa_bt_backend_set_implementation(&backend->this, &backend_impl, backend);

	backend->this.name = "ofono";
	backend->this.exclusive = true;
	backend->monitor = monitor;
	backend->quirks = quirks;
	backend->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	backend->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	backend->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	backend->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);
	backend->loop_utils = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_LoopUtils);
	backend->conn = dbus_connection;
	if (info && (str = spa_dict_lookup(info, "bluez5.enable-msbc")))
		backend->msbc_supported = spa_atob(str);
	else
		backend->msbc_supported = false;

	spa_log_topic_init(backend->log, &log_topic);

	backend->timer = spa_loop_utils_add_timer(backend->loop_utils, activate_timer_event, backend);
	if (backend->timer == NULL) {
		free(backend);
		return NULL;
	}

	if (!dbus_connection_register_object_path(backend->conn,
						  OFONO_AUDIO_CLIENT,
						  &vtable_profile, backend)) {
		free(backend);
		return NULL;
	}

	if (add_filters(backend) < 0) {
		dbus_connection_unregister_object_path(backend->conn, OFONO_AUDIO_CLIENT);
		free(backend);
		return NULL;
	}

	backend->this.available = is_available(backend);

	return &backend->this;
}
