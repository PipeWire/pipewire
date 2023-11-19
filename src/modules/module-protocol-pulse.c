/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>

#include <pipewire/impl.h>

#include "module-protocol-pulse/pulse-server.h"

/** \page page_module_protocol_pulse Protocol Pulse
 *
 * This module implements a complete PulseAudio server on top of
 * PipeWire.  This is only the server implementation, client are expected
 * to use the original PulseAudio client library. This provides a
 * high level of compatibility with existing applications; in fact,
 * all usual PulseAudio tools such as pavucontrol, pactl, pamon, paplay
 * should continue to work as they did before.
 *
 * This module is usually loaded as part of a standalone pipewire process,
 * called pipewire-pulse, with the pipewire-pulse.conf config file.
 *
 * The pulse server implements a sample cache that is otherwise not
 * available in PipeWire.
 *
 * ## Module Name
 *
 * `libpipewire-module-protocol-pulse`
 *
 * ## Module Options
 *
 * The module arguments can be the contents of the pulse.properties but
 * it is recommended to make a separate pulse.properties section in the
 * config file so that overrides can be done.
 *
 * ## pulse.properties
 *
 * A config section with server properties can be given.
 *
 *\code{.unparsed}
 * pulse.properties = {
 *     # the addresses this server listens on
 *     server.address = [
 *         "unix:native"
 *         #"unix:/tmp/something"              # absolute paths may be used
 *         #"tcp:4713"                         # IPv4 and IPv6 on all addresses
 *         #"tcp:[::]:9999"                    # IPv6 on all addresses
 *         #"tcp:127.0.0.1:8888"               # IPv4 on a single address
 *         #
 *         #{ address = "tcp:4713"             # address
 *         #  max-clients = 64                 # maximum number of clients
 *         #  listen-backlog = 32              # backlog in the server listen queue
 *         #  client.access = "restricted"     # permissions for clients
 *         #}
 *     ]
 *     #pulse.min.req          = 128/48000     # 2.7ms
 *     #pulse.default.req      = 960/48000     # 20 milliseconds
 *     #pulse.min.frag         = 128/48000     # 2.7ms
 *     #pulse.default.frag     = 96000/48000   # 2 seconds
 *     #pulse.default.tlength  = 96000/48000   # 2 seconds
 *     #pulse.min.quantum      = 128/48000     # 2.7ms
 *     #pulse.default.format   = F32
 *     #pulse.default.position = [ FL FR ]
 *     # These overrides are only applied when running in a vm.
 *     vm.overrides = {
 *         pulse.min.quantum = 1024/48000      # 22ms
 *     }
 * }
 *\endcode
 *
 * ### Connection options
 *
 *\code{.unparsed}
 *     ...
 *     server.address = [
 *         "unix:native"
 *         # "tcp:4713"
 *     ]
 *     ...
 *\endcode
 *
 * The addresses the server listens on when starting. Uncomment the `tcp:4713` entry to also
 * make the server listen on a tcp socket. This is equivalent to loading `libpipewire-module-native-protocol-tcp`.
 *
 * There is also a slightly more verbose syntax with more options:
 *
 *\code{.unparsed}
 *     ....
 *     server.address = [
 *       {  address = "tcp:4713"             # address
 *          max-clients = 64                 # maximum number of clients
 *          listen-backlog = 32              # backlog in the server listen queue
 *          client.access = "restricted"     # permissions for clients
 *       }
 *     ....
 *\endcode
 *
 * Use `client.access` to use one of the access methods to restrict the permissions given to
 * clients connected via this address.
 *
 * By default network access is given the "restricted" permissions. The session manager is responsible
 * for assigning permission to clients with restricted permissions (usually read-only permissions).
 *
 * ### Playback buffering options
 *
 *\code{.unparsed}
 *     pulse.min.req = 128/48000              # 2.7ms
 *\endcode
 *
 * The minimum amount of data to request for clients. The client requested
 * values will be clamped to this value. Lowering this value together with
 * tlength can decrease latency if the client wants this, but increase CPU overhead.
 *
 *\code{.unparsed}
 *     pulse.default.req = 960/48000          # 20 milliseconds
 *\endcode
 *
 * The default amount of data to request for clients. If the client does not
 * specify any particular value, this default will be used. Lowering this value
 * together with tlength can decrease latency but increase CPU overhead.
 *
 *\code{.unparsed}
 *     pulse.default.tlength = 96000/48000    # 2 seconds
 *\endcode
 *
 * The target amount of data to buffer on the server side. If the client did not
 * specify a value, this default will be used. Lower values can decrease the
 * latency.
 *
 * ### Record buffering options
 *
 *\code{.unparsed}
 *     pulse.min.frag = 128/48000             # 2.7ms
 *\endcode
 *
 * The minimum allowed size of the capture buffer before it is sent to a client.
 * The requested value of the client will be clamped to this. Lowering this value
 * can reduce latency at the expense of more CPU usage.
 *
 *\code{.unparsed}
 *     pulse.default.frag = 96000/48000       # 2 seconds
 *\endcode
 *
 * The default size of the capture buffer before it is sent to a client. If the client
 * did not specify any value, this default will be used. Lowering this value can
 * reduce latency at the expense of more CPU usage.
 *
 * ### Scheduling options
 *
 *\code{.unparsed}
 *     pulse.min.quantum = 128/48000          # 2.7ms
 *\endcode
 *
 * The minimum quantum (buffer size in samples) to use for pulseaudio clients.
 * This value is calculated based on the frag and req/tlength for record and
 * playback streams respectively and then clamped to this value to ensure no
 * pulseaudio client asks for too small quantums. Lowering this value might
 * decrease latency at the expense of more CPU usage.
 *
 * ### Format options
 *
 *\code{.unparsed}
 *     pulse.default.format = F32
 *\endcode
 *
 * Some modules will default to this format when no other format was given. This
 * is equivalent to the PulseAudio `default-sample-format` option in
 * `/etc/pulse/daemon.conf`.
 *
 *\code{.unparsed}
 *     pulse.default.position = [ FL FR ]
 *\endcode
 *
 * Some modules will default to this channelmap (with its number of channels).
 * This is equivalent to the PulseAudio `default-sample-channels` and
 * `default-channel-map` options in `/etc/pulse/daemon.conf`.
 *
 * ### VM options
 *
 *\code{.unparsed}
 *     vm.overrides = {
 *         pulse.min.quantum = 1024/48000      # 22ms
 *     }
 *\endcode
 *
 * When running in a VM, the `vm.override` section will override the properties
 * in pulse.properties with the given values. This might be interesting because
 * VMs usually can't support the low latency settings that are possible on real
 * hardware.
 *
 * ### Quirk options
 *
 *\code{.unparsed}
 *     pulse.fix.format = "S16LE"
 *\endcode
 *
 * When a stream uses the FIX_FORMAT flag, fixate the format to this value.
 * Normally the format would be fixed to the sink/source that the stream connects
 * to. When an invalid format (null or "") is set, the FIX_FORMAT flag is ignored.
 *
 *\code{.unparsed}
 *     pulse.fix.rate = 48000
 *\endcode
 *
 * When a stream uses the FIX_RATE flag, fixate the sample rate to this value.
 * Normally the rate would be fixed to the sink/source that the stream connects
 * to. When a 0 rate is set, the FIX_RATE flag is ignored.
 *
 *\code{.unparsed}
 *     pulse.fix.position = "[ FL FR ]"
 *\endcode
 *
 * When a stream uses the FIX_CHANNELS flag, fixate the channels to this value.
 * Normally the channels would be fixed to the sink/source that the stream connects
 * to. When an invalid position (null or "") is set, the FIX_CHANNELS flag is ignored.
 *
 * ## Command execution
 *
 * As part of the server startup sequence, a set of commands can be executed.
 * Currently, this can be used to load additional modules into the server.
 *
 *\code{.unparsed}
 * # Extra commands can be executed here.
 * #   load-module : loads a module with args and flags
 * #      args = "<module-name> <module-args>"
 * #      flags = [ "no-fail" ]
 * pulse.cmd = [
 *     { cmd = "load-module" args = "module-always-sink" flags = [ ] }
 *     #{ cmd = "load-module" args = "module-switch-on-connect" }
 *     #{ cmd = "load-module" args = "module-gsettings" flags = [ "nofail" ] }
 * ]
 *\endcode
 *
 * ## Stream settings and rules
 *
 * Streams created by module-protocol-pulse will use the stream.properties
 * section and stream.rules sections as usual.
 *
 * ## Application settings (Rules)
 *
 * The pulse protocol module supports generic config rules. It supports a pulse.rules
 * section with a `quirks` and an `update-props` action.
 *
 *\code{.unparsed}
 * pulse.rules = [
 *     {
 *         # skype does not want to use devices that don't have an S16 sample format.
 *         matches = [
 *              { application.process.binary = "teams" }
 *              { application.process.binary = "teams-insiders" }
 *              { application.process.binary = "skypeforlinux" }
 *         ]
 *         actions = { quirks = [ force-s16-info ] }
 *     }
 *     {
 *         # speech dispatcher asks for too small latency and then underruns.
 *         matches = [ { application.name = "~speech-dispatcher*" } ]
 *         actions = {
 *             update-props = {
 *                 pulse.min.req          = 1024/48000     # 21ms
 *                 pulse.min.quantum      = 1024/48000     # 21ms
 *             }
 *         }
 *     }
 * ]
 *\endcode
 *
 * ### Quirks
 *
 * The quirks action takes an array of quirks to apply for the client.
 *
 * * `force-s16-info` makes the sink and source introspect code pretend that the sample format
 *                    is S16 (16 bits) samples.  Some application refuse the sink/source if this
 *                    is not the case.
 * * `remove-capture-dont-move` Removes the DONT_MOVE flag on capture streams. Some applications
 *                    set this flag so that the stream can't be moved anymore with tools such as
 *                    pavucontrol.
 * * `block-source-volume` blocks the client from updating any source volumes. This can be used
 *                    to disable things like automatic gain control.
 * * `block-sink-volume` blocks the client from updating any sink volumes.
 *
 * ### update-props
 *
 * Takes an object with the properties to update on the client. Common actions are to
 * tweak the quantum values.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-protocol-pulse
 *     args = { }
 * }
 * ]
 *
 * pulse.properties = {
 *     server.address = [ "unix:native" ]
 * }
 *
 * pulse.rules = [
 *     {
 *        # skype does not want to use devices that don't have an S16 sample format.
 *        matches = [
 *             { application.process.binary = "teams" }
 *             { application.process.binary = "teams-insiders" }
 *             { application.process.binary = "skypeforlinux" }
 *        ]
 *        actions = { quirks = [ force-s16-info ] }
 *    }
 *    {
 *        # speech dispatcher asks for too small latency and then underruns.
 *        matches = [ { application.name = "~speech-dispatcher*" } ]
 *        actions = {
 *            update-props = {
 *                pulse.min.req          = 1024/48000     # 21ms
 *                pulse.min.quantum      = 1024/48000     # 21ms
 *            }
 *        }
 *    }
 * ]
 *\endcode
 */

#define NAME "protocol-pulse"

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic
PW_LOG_TOPIC(pulse_conn, "conn." NAME);
PW_LOG_TOPIC(pulse_ext_dev_restore, "mod." NAME ".device-restore");
PW_LOG_TOPIC(pulse_ext_stream_restore, "mod." NAME ".stream-restore");

#define MODULE_USAGE	PW_PROTOCOL_PULSE_USAGE

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Implement a PulseAudio server" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct spa_hook module_listener;

	struct pw_protocol_pulse *pulse;
};

static void impl_free(struct impl *impl)
{
	spa_hook_remove(&impl->module_listener);
	if (impl->pulse)
		pw_protocol_pulse_destroy(impl->pulse);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug("module %p: destroy", impl);
	impl_free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);
	PW_LOG_TOPIC_INIT(pulse_conn);
	/* it's easier to init these here than adding an init() call to the
	 * extensions */
	PW_LOG_TOPIC_INIT(pulse_ext_dev_restore);
	PW_LOG_TOPIC_INIT(pulse_ext_stream_restore);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = NULL;

	impl->pulse = pw_protocol_pulse_new(context, props, 0);
	if (impl->pulse == NULL) {
		res = -errno;
		free(impl);
		return res;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
