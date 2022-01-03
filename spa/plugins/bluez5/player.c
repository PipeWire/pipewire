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

#include <errno.h>
#include <stdbool.h>
#include <dbus/dbus.h>

#include <spa/utils/string.h>

#include "defs.h"
#include "player.h"

#define PLAYER_OBJECT_PATH_BASE	"/media_player"

#define PLAYER_INTERFACE	"org.mpris.MediaPlayer2.Player"

#define PLAYER_INTROSPECT_XML						\
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE			\
	"<node>"							\
	" <interface name='" PLAYER_INTERFACE "'>"			\
	"  <property name='PlaybackStatus' type='s' access='read'/>"	\
	" </interface>"							\
	" <interface name='" DBUS_INTERFACE_PROPERTIES "'>"		\
	"   <method name='Get'>"					\
	"     <arg name='interface' type='s' direction='in' />"		\
	"     <arg name='name' type='s' direction='in' />"		\
	"     <arg name='value' type='v' direction='out' />"		\
	"   </method>"							\
	"   <method name='Set'>"					\
	"     <arg name='interface' type='s' direction='in' />"		\
	"     <arg name='name' type='s' direction='in' />"		\
	"     <arg name='value' type='v' direction='in' />"		\
	"   </method>"							\
	"   <method name='GetAll'>"					\
	"     <arg name='interface' type='s' direction='in' />"		\
	"     <arg name='properties' type='a{sv}' direction='out' />"	\
	"   </method>"							\
	"   <signal name='PropertiesChanged'>"				\
	"     <arg name='interface' type='s' />"			\
	"     <arg name='changed_properties' type='a{sv}' />"		\
	"     <arg name='invalidated_properties' type='as' />"		\
	"   </signal>"							\
	" </interface>"							\
	" <interface name='" DBUS_INTERFACE_INTROSPECTABLE "'>"		\
	"  <method name='Introspect'>"					\
	"   <arg name='xml' type='s' direction='out'/>"			\
	"  </method>"							\
	" </interface>"							\
	"</node>"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.player");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define MAX_PROPERTIES 1

struct impl {
	struct spa_bt_player this;
	DBusConnection *conn;
	char *path;
	struct spa_log *log;
	struct spa_dict_item properties_items[MAX_PROPERTIES];
	struct spa_dict properties;
	unsigned int playing_count;
};

static size_t instance_counter = 0;

static DBusMessage *properties_get(struct impl *impl, DBusMessage *m)
{
	const char *iface, *name;
	size_t j;

        if (!dbus_message_get_args(m, NULL,
                                        DBUS_TYPE_STRING, &iface,
                                        DBUS_TYPE_STRING, &name,
                                        DBUS_TYPE_INVALID))
                return NULL;

	if (!spa_streq(iface, PLAYER_INTERFACE))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"No such interface");

	for (j = 0; j < impl->properties.n_items; ++j) {
		const struct spa_dict_item *item = &impl->properties.items[j];
		if (spa_streq(item->key, name)) {
			DBusMessage *r;
			DBusMessageIter i, v;

			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;

			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					"s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING,
					&item->value);
			dbus_message_iter_close_container(&i, &v);
			return r;
		}
	}

	return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
			"No such property");
}

static void append_properties(struct impl *impl, DBusMessageIter *i)
{
	DBusMessageIter d, e, v;
	size_t j;

        dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY,
                        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                        DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
                        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &d);

	for (j = 0; j < impl->properties.n_items; ++j) {
		const struct spa_dict_item *item = &impl->properties.items[j];

		spa_log_debug(impl->log, "player %s: %s=%s", impl->path,
				item->key, item->value);

		dbus_message_iter_open_container(&d, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &item->key);
		dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
		dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &item->value);
		dbus_message_iter_close_container(&e, &v);
		dbus_message_iter_close_container(&d, &e);
	}

	dbus_message_iter_close_container(i, &d);
}

static DBusMessage *properties_get_all(struct impl *impl, DBusMessage *m)
{
	const char *iface, *name;
	DBusMessage *r;
	DBusMessageIter i;

        if (!dbus_message_get_args(m, NULL,
                                        DBUS_TYPE_STRING, &iface,
                                        DBUS_TYPE_STRING, &name,
                                        DBUS_TYPE_INVALID))
                return NULL;

	if (!spa_streq(iface, PLAYER_INTERFACE))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"No such interface");

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	append_properties(impl, &i);
	return r;
}

static DBusMessage *properties_set(struct impl *impl, DBusMessage *m)
{
	return dbus_message_new_error(m, DBUS_ERROR_PROPERTY_READ_ONLY,
			"Property not writable");
}

static DBusMessage *introspect(struct impl *impl, DBusMessage *m)
{
	const char *xml = PLAYER_INTROSPECT_XML;
	DBusMessage *r;
	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;
	if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
		return NULL;
	return r;
}

static DBusHandlerResult player_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct impl *impl = userdata;
	DBusMessage *r;

	if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
		r = introspect(impl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Get")) {
		r = properties_get(impl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
		r = properties_get_all(impl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
		r = properties_set(impl, m);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(impl->conn, r, NULL)) {
		dbus_message_unref(r);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int send_update_signal(struct impl *impl)
{
	DBusMessage *m;
	const char *iface = PLAYER_INTERFACE;
	DBusMessageIter i, a;
	int res = 0;

	m = dbus_message_new_signal(impl->path, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &iface);

	append_properties(impl, &i);

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &a);
        dbus_message_iter_close_container(&i, &a);

	if (!dbus_connection_send(impl->conn, m, NULL))
		res = -EIO;

	dbus_message_unref(m);

	return res;
}

static void update_properties(struct impl *impl, bool send_signal)
{
	int nitems = 0;

	switch (impl->this.state) {
	case SPA_BT_PLAYER_PLAYING:
		impl->properties_items[nitems++] = SPA_DICT_ITEM_INIT("PlaybackStatus", "Playing");
		break;
	case SPA_BT_PLAYER_STOPPED:
		impl->properties_items[nitems++] = SPA_DICT_ITEM_INIT("PlaybackStatus", "Stopped");
		break;
	}
	impl->properties = SPA_DICT_INIT(impl->properties_items, nitems);

	if (!send_signal)
		return;

	send_update_signal(impl);
}

struct spa_bt_player *spa_bt_player_new(void *dbus_connection, struct spa_log *log)
{
	struct impl *impl;
	const DBusObjectPathVTable vtable = {
		.message_function = player_handler,
	};

	spa_log_topic_init(log, &log_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->this.state = SPA_BT_PLAYER_STOPPED;
	impl->conn = dbus_connection;
	impl->log = log;
	impl->path = spa_aprintf("%s%zu", PLAYER_OBJECT_PATH_BASE, instance_counter++);
	if (impl->path == NULL) {
		free(impl);
		return NULL;
	}

	dbus_connection_ref(impl->conn);

	update_properties(impl, false);

	if (!dbus_connection_register_object_path(impl->conn, impl->path, &vtable, impl)) {
		spa_bt_player_destroy(&impl->this);
		errno = EIO;
		return NULL;
	}

	return &impl->this;
}

void spa_bt_player_destroy(struct spa_bt_player *player)
{
	struct impl *impl = SPA_CONTAINER_OF(player, struct impl, this);

	/*
	 * We unregister only the object path, but don't unregister it from
	 * BlueZ, to avoid hanging on BlueZ DBus activation. The assumption is
	 * that the DBus connection is terminated immediately after.
	 */
	dbus_connection_unregister_object_path(impl->conn, impl->path);

	dbus_connection_unref(impl->conn);
	free(impl->path);
	free(impl);
}

int spa_bt_player_set_state(struct spa_bt_player *player, enum spa_bt_player_state state)
{
	struct impl *impl = SPA_CONTAINER_OF(player, struct impl, this);

	switch (state) {
	case SPA_BT_PLAYER_PLAYING:
		if (impl->playing_count++ > 0)
			return 0;
		break;
	case SPA_BT_PLAYER_STOPPED:
		if (impl->playing_count == 0)
			return -EINVAL;
		if (--impl->playing_count > 0)
			return 0;
		break;
	default:
		return -EINVAL;
	}

	impl->this.state = state;
	update_properties(impl, true);

	return 0;
}

int spa_bt_player_register(struct spa_bt_player *player, const char *adapter_path)
{
	struct impl *impl = SPA_CONTAINER_OF(player, struct impl, this);

	DBusError err;
	DBusMessageIter i;
	DBusMessage *m, *r;
	int res = 0;

	spa_log_debug(impl->log, "RegisterPlayer() for dummy AVRCP player %s for %s",
			impl->path, adapter_path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, adapter_path,
			BLUEZ_MEDIA_INTERFACE, "RegisterPlayer");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &impl->path);
	append_properties(impl, &i);

	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(impl->conn, m, -1, &err);
	dbus_message_unref(m);

	if (r == NULL) {
		spa_log_error(impl->log, "RegisterPlayer() failed (%s)", err.message);
		dbus_error_free(&err);
		return -EIO;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(impl->log, "RegisterPlayer() failed");
		res = -EIO;
	}

	dbus_message_unref(r);

	return res;
}

int spa_bt_player_unregister(struct spa_bt_player *player, const char *adapter_path)
{
	struct impl *impl = SPA_CONTAINER_OF(player, struct impl, this);

	DBusError err;
	DBusMessageIter i;
	DBusMessage *m, *r;
	int res = 0;

	spa_log_debug(impl->log, "UnregisterPlayer() for dummy AVRCP player %s for %s",
			impl->path, adapter_path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE, adapter_path,
			BLUEZ_MEDIA_INTERFACE, "UnregisterPlayer");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &impl->path);

	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(impl->conn, m, -1, &err);
	dbus_message_unref(m);

	if (r == NULL) {
		spa_log_error(impl->log, "UnregisterPlayer() failed (%s)", err.message);
		dbus_error_free(&err);
		return -EIO;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(impl->log, "UnregisterPlayer() failed");
		res = -EIO;
	}

	dbus_message_unref(r);

	return res;
}
