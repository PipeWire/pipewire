/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-json.h>
#include <spa/debug/types.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include "module-protocol-pulse/format.h"
#include "module-zeroconf-discover/avahi-poll.h"

#include "network-utils.h"

/** \page page_module_snapcast_discover Snapcast Discover
 *
 * Automatically creates a Snapcast sink device based on zeroconf
 * information.
 *
 * This module will load module-protocol-simple for each announced stream that matches
 * the rule with the create-stream action and passes the properties to the module.
 *
 * If no stream.rules are given, it will create a sink for all announced
 * snapcast servers.
 *
 * A new stream will be created on the snapcast server with the given
 * `snapcast.stream-name` or `PipeWire-<hostname>`. You will need to route this new
 * stream to clients with the snapcast control application.
 *
 * ## Module Name
 *
 * `libpipewire-module-snapcast-discover`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `snapcast.discover-local` = allow discovery of local services as well.
 *    false by default.
 * - `stream.rules` = <rules>: match rules, use create-stream actions. See
 *   \ref page_module_protocol_simple for module properties.
 *
 * ### stream.rules matches
 *
 *  - `snapcast.ip`: the IP address of the snapcast server
 *  - `snapcast.port`: the port of the snapcast server
 *  - `snapcast.ifindex`: the interface index where the snapcast announcement
 *                        was received.
 *  - `snapcast.ifname`: the interface name where the snapcast announcement
 *                        was received.
 *  - `snapcast.name`: the name of the snapcast server
 *  - `snapcast.hostname`: the hostname of the snapcast server
 *  - `snapcast.domain`: the domain of the snapcast server
 *
 * ### stream.rules create-stream
 *
 * In addition to all the properties that can be passed to
 * \ref page_module_protocol_simple, you can also set:
 *
 * - `snapcast.stream-name`: The name of the stream on a snapcast server.
 * - `node.name`: The name of the sink that is created on the sender.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-snapcast-discover.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-snapcast-discover
 *     args = {
 *         stream.rules = [
 *             {   matches = [
 *                     {    snapcast.ip = "~.*"
 *                          #snapcast.port = 1000
 *                          #snapcast.ifindex = 1
 *                          #snapcast.ifname = eth0
 *                          #snapcast.name = ""
 *                          #snapcast.hostname = ""
 *                          #snapcast.domain = ""
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #audio.rate = 44100
 *                         #audio.format = S16LE   # S16LE, S24_32LE, S32LE
 *                         #audio.channels = 2
 *                         #audio.position = [ FL FR ]
 *                         #
 *                         # The stream name as is appears on the snapcast
 *                         # server:
 *                         #snapcast.stream-name = "PipeWire"
 *                         #
 *                         # The name of the sink on the sender:
 *                         #node.name = "Snapcast Sink"
 *                         #
 *                         #capture = true
 *                         #server.address = [ "tcp:4711" ]
 *                         #capture.props = {
 *                             #target.object = ""
 *                             #node.latency = 2048/48000
 *                             #media.class = "Audio/Sink"
 *                         #}
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * \ref page_module_protocol_simple
 */

#define NAME "snapcast-discover"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE "( stream.rules=<rules>, use create-stream actions )"

#define DEFAULT_FORMAT "S16LE"
#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { snapcast.ip = \"~.*\" } ] actions = { create-stream = { } } } ] "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Discover remote Snapcast streams" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define SERVICE_TYPE_CONTROL "_snapcast-jsonrpc._tcp"

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *properties;
	bool discover_local;
	struct pw_loop *loop;

	AvahiPoll *avahi_poll;
	AvahiClient *client;
	AvahiServiceBrowser *sink_browser;

	struct spa_list tunnel_list;
	uint32_t id;
};

struct tunnel_info {
	const char *name;
	const char *host;
	uint16_t port;
};

#define TUNNEL_INFO(...) ((struct tunnel_info){ __VA_ARGS__ })

struct tunnel {
	struct impl *impl;
	struct spa_list link;
	struct tunnel_info info;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	char *server_address;
	char *stream_name;
	struct spa_audio_info_raw audio_info;
	struct spa_source *source;
	bool connecting;
	bool need_flush;
};

static int start_client(struct impl *impl);

static struct tunnel *make_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;

	t->info.name = info->name ? strdup(info->name) : NULL;
	t->info.host = info->host ? strdup(info->host) : NULL;
	t->info.port = info->port;
	t->impl = impl;
	spa_list_append(&impl->tunnel_list, &t->link);

	return t;
}

static struct tunnel *find_tunnel(struct impl *impl, const struct tunnel_info *info)
{
	struct tunnel *t;
	spa_list_for_each(t, &impl->tunnel_list, link) {
		if (spa_streq(t->info.name, info->name))
			return t;
	}
	return NULL;
}

static void free_tunnel(struct tunnel *t)
{
	spa_list_remove(&t->link);
	if (t->module)
		pw_impl_module_destroy(t->module);
	free((char *) t->info.name);
	free((char *) t->info.host);
	free(t->server_address);
	free(t->stream_name);
	free(t);
}

static void impl_free(struct impl *impl)
{
	struct tunnel *t;

	spa_list_consume(t, &impl->tunnel_list, link)
		free_tunnel(t);

	if (impl->sink_browser)
		avahi_service_browser_free(impl->sink_browser);
	if (impl->client)
		avahi_client_free(impl->client);
	if (impl->avahi_poll)
		pw_avahi_poll_free(impl->avahi_poll);
	pw_properties_free(impl->properties);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void pw_properties_from_avahi_string(const char *key, const char *value,
		struct pw_properties *props)
{
}

static void submodule_destroy(void *data)
{
	struct tunnel *t = data;
	spa_hook_remove(&t->module_listener);
	t->module = NULL;
}

static const struct pw_impl_module_events submodule_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = submodule_destroy,
};

static int snapcast_disconnect(struct tunnel *t)
{
	if (t->source)
		pw_loop_destroy_source(t->impl->loop, t->source);
	t->source = NULL;
	return 0;
}

static int get_bps(uint32_t format)
{
	switch (format) {
	case SPA_AUDIO_FORMAT_S16_LE:
		return 16;
	case SPA_AUDIO_FORMAT_S24_32_LE:
		return 24;
	case SPA_AUDIO_FORMAT_S32_LE:
		return 32;
	default:
		return 0;
	}
}

static int handle_connect(struct tunnel *t, int fd)
{
	int res;
	socklen_t len;
	char *str;
	struct impl *impl = t->impl;

	len = sizeof(res);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &len) < 0) {
		pw_log_error("getsockopt: %m");
		return -errno;
	}
	if (res != 0)
		return -res;

	t->connecting = false;
	pw_log_info("connected");

	str = spa_aprintf("{\"id\":%u,\"jsonrpc\": \"2.0\",\"method\":\"Server.GetRPCVersion\"}\r\n",
			impl->id++);
	res = write(t->source->fd, str, strlen(str));
	pw_log_info("wrote %s: %d", str, res);
	free(str);

	str = spa_aprintf("{\"id\":%u,\"jsonrpc\":\"2.0\",\"method\":\"Stream.RemoveStream\","
			"\"params\":{\"id\":\"%s\"}}\r\n", impl->id++, t->stream_name);
	res = write(t->source->fd, str, strlen(str));
	pw_log_info("wrote %s: %d", str, res);
	free(str);

	str = spa_aprintf("{\"id\":%u,\"jsonrpc\":\"2.0\",\"method\":\"Stream.AddStream\""
		",\"params\":{\"streamUri\":\"tcp://%s?name=%s&mode=client&"
		"sampleformat=%d:%d:%d&codec=pcm&chunk_ms=20\"}}\r\n", impl->id++,
		t->server_address, t->stream_name, t->audio_info.rate,
		get_bps(t->audio_info.format), t->audio_info.channels);
	res = write(t->source->fd, str, strlen(str));
	pw_log_info("wrote %s: %d", str, res);
	free(str);

	return 0;
}

static int process_input(struct tunnel *t)
{
	char buffer[1024] = "";
	int res = 0;

	while (true) {
		res = read(t->source->fd, buffer, sizeof(buffer));
		if (res == 0)
			return -EPIPE;
		if (res < 0) {
			res = -errno;
			if (res == -EINTR)
				continue;
			if (res != -EAGAIN && res != -EWOULDBLOCK)
				return res;
			break;
		}
	}

	pw_log_info("received: %s", buffer);
	return 0;
}

static int flush_output(struct tunnel *t)
{
	t->need_flush = false;
	return 0;
}

static void
on_source_io(void *data, int fd, uint32_t mask)
{
	struct tunnel *t = data;
	int res;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = process_input(t)) < 0)
			goto error;
	}
	if (mask & SPA_IO_OUT || t->need_flush) {
		if (t->connecting) {
			if ((res = handle_connect(t, fd)) < 0)
				goto error;
		}
		res = flush_output(t);
		if (res >= 0) {
			pw_loop_update_io(t->impl->loop, t->source,
				t->source->mask & ~SPA_IO_OUT);
		} else if (res != -EAGAIN)
			goto error;
	}
done:
	return;
error:
	pw_log_error("%p: got connection error %d (%s)", t, res, spa_strerror(res));
	snapcast_disconnect(t);
	goto done;
}


static int snapcast_connect(struct tunnel *t)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int res, fd;
	char port_str[12];

	if (t->server_address == NULL)
		return 0;

	if (t->source != NULL)
		snapcast_disconnect(t);

	pw_log_info("%p: connect %s:%u", t, t->info.host, t->info.port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	spa_scnprintf(port_str, sizeof(port_str), "%u", t->info.port);

	if ((res = getaddrinfo(t->info.host, port_str, &hints, &result)) != 0) {
		pw_log_error("getaddrinfo: %s", gai_strerror(res));
		return -EINVAL;
	}
	res = -ENOENT;
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family,
				rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				rp->ai_protocol);
		if (fd == -1)
			continue;

		res = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (res == 0 || (res < 0 && errno == EINPROGRESS))
			break;

		res = -errno;
		close(fd);
	}
	freeaddrinfo(result);

	if (rp == NULL) {
		pw_log_error("Could not connect to %s:%u: %s", t->info.host, t->info.port,
				spa_strerror(res));
		return -EINVAL;
	}

	t->source = pw_loop_add_io(t->impl->loop, fd,
			SPA_IO_IN | SPA_IO_OUT | SPA_IO_HUP | SPA_IO_ERR,
			true, on_source_io, t);

	if (t->source == NULL) {
		res = -errno;
		pw_log_error("%p: source create failed: %m", t);
		close(fd);
		return res;
	}
	t->connecting = true;
	pw_log_info("%p: connecting", t);

	return 0;

}

static int add_snapcast_stream(struct impl *impl, struct tunnel *t,
		struct pw_properties *props, const char *servers)
{
	struct spa_json it[1];
	char v[256];

        if (spa_json_begin_array_relax(&it[0], servers, strlen(servers)) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[0], v, sizeof(v)) > 0) {
		t->server_address = strdup(v);
		snapcast_connect(t);
		return 0;
	}
	return -ENOENT;
}

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, DEFAULT_FORMAT),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_RATE, SPA_STRINGIFY(DEFAULT_RATE)),
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_POSITION, DEFAULT_POSITION)),
			&props->dict,
			SPA_KEY_AUDIO_FORMAT,
			SPA_KEY_AUDIO_RATE,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL);

	pw_properties_set(props, PW_KEY_AUDIO_FORMAT,
			spa_type_audio_format_to_short_name(info->format));
	pw_properties_setf(props, PW_KEY_AUDIO_RATE, "%d", info->rate);
	pw_properties_setf(props, PW_KEY_AUDIO_CHANNELS, "%d", info->channels);
}

static int create_stream(struct impl *impl, struct pw_properties *props,
		struct tunnel *t)
{
	FILE *f;
	char *args;
	size_t size;
	int res = 0;
	struct pw_impl_module *mod;
	const struct pw_properties *mod_props;
	const char *str;

	if ((str = pw_properties_get(props, "snapcast.stream-name")) == NULL)
		pw_properties_setf(props, "snapcast.stream-name",
				"PipeWire-%s", pw_get_host_name());
	if ((str = pw_properties_get(props, "snapcast.stream-name")) == NULL)
		str = "PipeWire";
	t->stream_name = strdup(str);

	if ((str = pw_properties_get(props, "capture")) == NULL)
		pw_properties_set(props, "capture", "true");
	if ((str = pw_properties_get(props, "capture.props")) == NULL)
		pw_properties_set(props, "capture.props", "{ media.class = Audio/Sink }");

	parse_audio_info(props, &t->audio_info);

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &props->dict, 0);
	fprintf(f, "}");
        fclose(f);

	pw_log_info("loading module args:'%s'", args);
	mod = pw_context_load_module(impl->context,
			"libpipewire-module-protocol-simple",
			args, NULL);
	free(args);

	if (mod == NULL) {
		res = -errno;
		pw_log_error("Can't load module: %m");
                goto done;
	}

	pw_impl_module_add_listener(mod, &t->module_listener, &submodule_events, t);
	t->module = mod;

	if ((mod_props = pw_impl_module_get_properties(mod)) != NULL) {
		const char *addr;
		if ((addr = pw_properties_get(mod_props, "server.address"))) {
			add_snapcast_stream(impl, t, props, addr);
		}
	}
done:
	return res;
}

struct match_info {
	struct impl *impl;
	struct pw_properties *props;
	struct tunnel *tunnel;
	bool matched;
};

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct match_info *i = data;
	int res = 0;

	i->matched = true;
	if (spa_streq(action, "create-stream")) {
		pw_properties_update_string(i->props, str, len);
		create_stream(i->impl, i->props, i->tunnel);
	}
	return res;
}

static void resolver_cb(AvahiServiceResolver *r, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiResolverEvent event, const char *name, const char *type, const char *domain,
	const char *host_name, const AvahiAddress *a, uint16_t port, AvahiStringList *txt,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel_info tinfo;
	struct tunnel *t;
	const char *str, *link_local_range = "169.254.";
	AvahiStringList *l;
	struct pw_properties *props = NULL;
	char at[AVAHI_ADDRESS_STR_MAX];
	char hbuf[NI_MAXHOST];
	char if_suffix[16] = "";
	struct ifreq ifreq;
	int res, family;

	if (event != AVAHI_RESOLVER_FOUND) {
		pw_log_error("Resolving of '%s' failed: %s", name,
				avahi_strerror(avahi_client_errno(impl->client)));
		goto done;
	}

	avahi_address_snprint(at, sizeof(at), a);
	if (spa_strstartswith(at, link_local_range)) {
		pw_log_info("found link-local ip address %s - skipping tunnel creation", at);
		goto done;
	}
	pw_log_info("%s %s", name, at);

	tinfo = TUNNEL_INFO(.name = name, .port = port);

	t = find_tunnel(impl, &tinfo);
	if (t == NULL)
		t = make_tunnel(impl, &tinfo);
	if (t == NULL) {
		pw_log_error("Can't make tunnel: %m");
		goto done;
	}
	if (t->module != NULL) {
		pw_log_info("found duplicate mdns entry for %s on IP %s - skipping tunnel creation", name, at);
		goto done;
	}

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		pw_log_error("Can't allocate properties: %m");
		goto done;
	}

	if (a->proto == AVAHI_PROTO_INET6 &&
	    a->data.ipv6.address[0] == 0xfe &&
	    (a->data.ipv6.address[1] & 0xc0) == 0x80)
		snprintf(if_suffix, sizeof(if_suffix), "%%%d", interface);

	pw_properties_setf(props, "snapcast.ip", "%s%s", at, if_suffix);
	pw_properties_setf(props, "snapcast.ifindex", "%d", interface);
	pw_properties_setf(props, "snapcast.port", "%u", port);
	pw_properties_setf(props, "snapcast.name", "%s", name);
	pw_properties_setf(props, "snapcast.hostname", "%s", host_name);
	pw_properties_setf(props, "snapcast.domain", "%s", domain);

	free((char*)t->info.host);
	t->info.host = strdup(pw_properties_get(props, "snapcast.ip"));

	family = protocol == AVAHI_PROTO_INET ? AF_INET : AF_INET6;

	spa_zero(ifreq);
	ifreq.ifr_ifindex = interface;
	if_indextoname(interface, ifreq.ifr_name);
	pw_properties_setf(props, "snapcast.ifname", "%s", ifreq.ifr_name);
	pw_properties_setf(props, "local.ifname", "%s", ifreq.ifr_name);

	struct ifaddrs *if_addr, *ifp;
	if (getifaddrs(&if_addr) < 0)
		pw_log_error("error: %m");

	for (ifp = if_addr; ifp != NULL; ifp = ifp->ifa_next) {
		if (ifp->ifa_addr == NULL)
			continue;

		if (spa_streq(ifp->ifa_name, ifreq.ifr_name) &&
		    ifp->ifa_addr->sa_family == family) {
			break;
		}
	}
	if (ifp != NULL) {
		if ((res = getnameinfo((struct sockaddr *)ifp->ifa_addr,
				(family == AF_INET) ? sizeof(struct sockaddr_in) :
						sizeof(struct sockaddr_in6),
				hbuf, sizeof(hbuf), NULL, 0, NI_NUMERICHOST)) == 0) {
			pw_properties_setf(props, "server.address", "[ \"tcp:%s%s%s:0\" ]",
					family == AF_INET ? "" : "[",
					hbuf,
					family == AF_INET ? "" : "]");
			pw_properties_setf(props, "local.ifaddress", "%s%s%s",
					family == AF_INET ? "" : "[",
					hbuf,
					family == AF_INET ? "" : "]");
		} else {
			pw_log_warn("error: %m %d %s", res, gai_strerror(res));
		}
	}
	freeifaddrs(if_addr);

	for (l = txt; l; l = l->next) {
		char *key, *value;

		if (avahi_string_list_get_pair(l, &key, &value, NULL) != 0)
			break;

		pw_properties_from_avahi_string(key, value, props);
		avahi_free(key);
		avahi_free(value);
	}

	if ((str = pw_properties_get(impl->properties, "stream.rules")) == NULL)
		str = DEFAULT_CREATE_RULES;
	if (str != NULL) {
		struct match_info minfo = {
			.impl = impl,
			.props = props,
			.tunnel = t,
		};
		pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
				rule_matched, &minfo);

		if (!minfo.matched)
			pw_log_info("unmatched service found %s", str);
	}

done:
	avahi_service_resolver_free(r);
	pw_properties_free(props);
}


static void browser_cb(AvahiServiceBrowser *b, AvahiIfIndex interface, AvahiProtocol protocol,
	AvahiBrowserEvent event, const char *name, const char *type, const char *domain,
	AvahiLookupResultFlags flags, void *userdata)
{
	struct impl *impl = userdata;
	struct tunnel_info info;
	struct tunnel *t;

	if ((flags & AVAHI_LOOKUP_RESULT_LOCAL) && !impl->discover_local)
		return;

	/* snapcast does not seem to work well with IPv6 */
	if (protocol == AVAHI_PROTO_INET6)
		return;

	info = TUNNEL_INFO(.name = name);

	t = find_tunnel(impl, &info);

	switch (event) {
	case AVAHI_BROWSER_NEW:
		if (t != NULL) {
			pw_log_info("found duplicate mdns entry - skipping tunnel creation");
			return;
		}
		if (!(avahi_service_resolver_new(impl->client,
						interface, protocol,
						name, type, domain,
						AVAHI_PROTO_UNSPEC, 0,
						resolver_cb, impl)))
			pw_log_error("can't make service resolver: %s",
					avahi_strerror(avahi_client_errno(impl->client)));
		break;
	case AVAHI_BROWSER_REMOVE:
		if (t == NULL)
			return;
		free_tunnel(t);
		break;
	default:
		break;
	}
}


static struct AvahiServiceBrowser *make_browser(struct impl *impl, const char *service_type)
{
	struct AvahiServiceBrowser *s;

	s = avahi_service_browser_new(impl->client,
                              AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                              service_type, NULL, 0,
                              browser_cb, impl);
	if (s == NULL) {
		pw_log_error("can't make browser for %s: %s", service_type,
				avahi_strerror(avahi_client_errno(impl->client)));
	}
	return s;
}

static void client_callback(AvahiClient *c, AvahiClientState state, void *userdata)
{
	struct impl *impl = userdata;

	impl->client = c;

	switch (state) {
	case AVAHI_CLIENT_S_REGISTERING:
	case AVAHI_CLIENT_S_RUNNING:
	case AVAHI_CLIENT_S_COLLISION:
		if (impl->sink_browser == NULL)
			impl->sink_browser = make_browser(impl, SERVICE_TYPE_CONTROL);
		if (impl->sink_browser == NULL)
			goto error;
		break;
	case AVAHI_CLIENT_FAILURE:
		if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED)
			start_client(impl);

		SPA_FALLTHROUGH;
	case AVAHI_CLIENT_CONNECTING:
		if (impl->sink_browser) {
			avahi_service_browser_free(impl->sink_browser);
			impl->sink_browser = NULL;
		}
		break;
	default:
		break;
	}
	return;
error:
	pw_impl_module_schedule_destroy(impl->module);
}

static int start_client(struct impl *impl)
{
	int res;
	if ((impl->client = avahi_client_new(impl->avahi_poll,
					AVAHI_CLIENT_NO_FAIL,
					client_callback, impl,
					&res)) == NULL) {
		pw_log_error("can't create client: %s", avahi_strerror(res));
		pw_impl_module_schedule_destroy(impl->module);
		return -EIO;
	}
	return 0;
}

static int start_avahi(struct impl *impl)
{

	impl->avahi_poll = pw_avahi_poll_new(impl->context);

	return start_client(impl);
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL)
		goto error_errno;

	spa_list_init(&impl->tunnel_list);

	impl->loop = pw_context_get_main_loop(context);
	impl->module = module;
	impl->context = context;
	impl->properties = props;

	impl->discover_local =  pw_properties_get_bool(impl->properties,
			"snapcast.discover-local", false);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	start_avahi(impl);

	return 0;

error_errno:
	res = -errno;
	if (impl)
		impl_free(impl);
	return res;
}
