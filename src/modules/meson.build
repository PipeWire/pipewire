subdir('spa')

# The list of "main" source files for modules, the ones that have the
# doxygen documentation
module_sources = [
  'module-access.c',
  'module-adapter.c',
  'module-client-device.c',
  'module-client-node.c',
  'module-echo-cancel.c',
  'module-example-sink.c',
  'module-example-source.c',
  'module-filter-chain.c',
  'module-link-factory.c',
  'module-loopback.c',
  'module-metadata.c',
  'module-portal.c',
  'module-profiler.c',
  'module-protocol-native.c',
  'module-protocol-pulse.c',
  'module-protocol-simple.c',
  'module-pulse-tunnel.c',
  'module-rt.c',
  'module-rtkit.c',
  'module-session-manager.c',
  'module-zeroconf-discover.c',
  'module-roc-source.c',
  'module-roc-sink.c',
]

pipewire_module_access = shared_library('pipewire-module-access', [ 'module-access.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_loopback = shared_library('pipewire-module-loopback',
  [ 'module-loopback.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_filter_chain = shared_library('pipewire-module-filter-chain',
  [ 'module-filter-chain.c',
    'module-filter-chain/biquad.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_echo_cancel_sources = [
  'module-echo-cancel.c',
  'module-echo-cancel/aec-null.c',
]

if webrtc_dep.found()
  pipewire_module_echo_cancel_sources += [
    'module-echo-cancel/aec-webrtc.cpp'
  ]
endif

pipewire_module_echo_cancel = shared_library('pipewire-module-echo-cancel',
  pipewire_module_echo_cancel_sources,
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, webrtc_dep],
)

pipewire_module_profiler = shared_library('pipewire-module-profiler',
  [ 'module-profiler.c',
    'module-profiler/protocol-native.c', ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

if build_machine.system() == 'linux'
pipewire_module_rt = shared_library('pipewire-module-rt', [ 'module-rt.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [dl_lib, pipewire_dep],
)
endif

if dbus_dep.found()
pipewire_module_rtkit = shared_library('pipewire-module-rtkit', [ 'module-rtkit.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
)
pipewire_module_portal = shared_library('pipewire-module-portal', [ 'module-portal.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
)
endif

pipewire_module_client_node = shared_library('pipewire-module-client-device',
  [ 'module-client-device.c',
    'module-client-device/resource-device.c',
    'module-client-device/proxy-device.c',
    'module-client-device/protocol-native.c', ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_link_factory = shared_library('pipewire-module-link-factory',
  [ 'module-link-factory.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_protocol_deps = [mathlib, dl_lib, pipewire_dep]

if systemd_dep.found()
  pipewire_module_protocol_deps += systemd_dep
endif

pipewire_module_protocol_native = shared_library('pipewire-module-protocol-native',
  [ 'module-protocol-native.c',
    'module-protocol-native/local-socket.c',
    'module-protocol-native/portal-screencast.c',
    'module-protocol-native/protocol-native.c',
    'module-protocol-native/v0/protocol-native.c',
    'module-protocol-native/connection.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_deps,
)

pipewire_module_protocol_pulse_deps = pipewire_module_protocol_deps

if dbus_dep.found()
  pipewire_module_protocol_pulse_deps += dbus_dep
endif

pipewire_module_protocol_pulse_sources = [
  'module-protocol-pulse.c',
  'module-protocol-pulse/client.c',
  'module-protocol-pulse/collect.c',
  'module-protocol-pulse/dbus-name.c',
  'module-protocol-pulse/extension.c',
  'module-protocol-pulse/format.c',
  'module-protocol-pulse/manager.c',
  'module-protocol-pulse/media-roles.c',
  'module-protocol-pulse/message.c',
  'module-protocol-pulse/message-handler.c',
  'module-protocol-pulse/module.c',
  'module-protocol-pulse/operation.c',
  'module-protocol-pulse/pending-sample.c',
  'module-protocol-pulse/pulse-server.c',
  'module-protocol-pulse/quirks.c',
  'module-protocol-pulse/reply.c',
  'module-protocol-pulse/sample.c',
  'module-protocol-pulse/sample-play.c',
  'module-protocol-pulse/server.c',
  'module-protocol-pulse/stream.c',
  'module-protocol-pulse/utils.c',
  'module-protocol-pulse/volume.c',
  'module-protocol-pulse/modules/module-combine-sink.c',
  'module-protocol-pulse/modules/module-echo-cancel.c',
  'module-protocol-pulse/modules/module-ladspa-sink.c',
  'module-protocol-pulse/modules/module-ladspa-source.c',
  'module-protocol-pulse/modules/module-loopback.c',
  'module-protocol-pulse/modules/module-native-protocol-tcp.c',
  'module-protocol-pulse/modules/module-null-sink.c',
  'module-protocol-pulse/modules/module-pipe-source.c',
  'module-protocol-pulse/modules/module-pipe-sink.c',
  'module-protocol-pulse/modules/module-remap-sink.c',
  'module-protocol-pulse/modules/module-remap-source.c',
  'module-protocol-pulse/modules/module-simple-protocol-tcp.c',
  'module-protocol-pulse/modules/module-tunnel-sink.c',
  'module-protocol-pulse/modules/module-tunnel-source.c',
  'module-protocol-pulse/modules/module-zeroconf-discover.c',
]

if avahi_dep.found()
  pipewire_module_protocol_pulse_sources += [
    'module-protocol-pulse/modules/module-zeroconf-publish.c',
    'module-zeroconf-discover/avahi-poll.c',
  ]
  pipewire_module_protocol_pulse_deps += avahi_dep
  cdata.set('HAVE_AVAHI', 1)
endif

roc_lib = cc.find_library('roc', required: get_option('roc'))
summary({'ROC': roc_lib.found()}, bool_yn: true, section: 'Streaming between daemons')
if roc_lib.found()
  pipewire_module_protocol_pulse_sources += [
    'module-protocol-pulse/modules/module-roc-sink.c',
    'module-protocol-pulse/modules/module-roc-source.c',
  ]
  pipewire_module_protocol_pulse_deps += roc_lib
  cdata.set('HAVE_ROC', 1)
endif

pipewire_module_protocol_pulse = shared_library('pipewire-module-protocol-pulse',
  pipewire_module_protocol_pulse_sources,
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_pulse_deps,
)

if pulseaudio_dep.found()
  pipewire_module_pulse_tunnel = shared_library('pipewire-module-pulse-tunnel',
  [ 'module-pulse-tunnel.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pulseaudio_dep],
)
endif

pipewire_module_protocol_simple = shared_library('pipewire-module-protocol-simple',
  [ 'module-protocol-simple.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_deps,
)

pipewire_module_example_sink = shared_library('pipewire-module-example-sink',
  [ 'module-example-sink.c' ],
  include_directories : [configinc, spa_inc],
  install : false,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_example_sink = shared_library('pipewire-module-example-source',
  [ 'module-example-source.c' ],
  include_directories : [configinc, spa_inc],
  install : false,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_client_node = shared_library('pipewire-module-client-node',
  [ 'module-client-node.c',
    'module-client-node/remote-node.c',
    'module-client-node/client-node.c',
    'module-client-node/protocol-native.c',
    'module-client-node/v0/client-node.c',
    'module-client-node/v0/transport.c',
    'module-client-node/v0/protocol-native.c',
    'spa/spa-node.c', ],
  include_directories : [configinc, spa_inc],
  link_with : pipewire_module_protocol_native,
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_metadata = shared_library('pipewire-module-metadata',
  [ 'module-metadata.c',
    'module-metadata/proxy-metadata.c',
    'module-metadata/metadata.c',
    'module-metadata/protocol-native.c'],
  include_directories : [configinc, spa_inc],
  link_with : pipewire_module_protocol_native,
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

test('pw-test-protocol-native',
	executable('pw-test-protocol-native',
		[ 'module-protocol-native/test-connection.c',
		  'module-protocol-native/connection.c' ],
			c_args : libpipewire_c_args,
			include_directories : [configinc, spa_inc ],
			dependencies : [pipewire_dep],
			install : installed_tests_enabled,
			install_dir : installed_tests_execdir),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
		'PIPEWIRE_CONFIG_DIR=@0@/src/daemon/'.format(meson.build_root()),
		'PIPEWIRE_MODULE_DIR=@0@/src/modules/'.format(meson.build_root())
	])

if installed_tests_enabled
  test_conf = configuration_data()
  test_conf.set('exec', installed_tests_execdir / 'pw-test-protocol-native')
  configure_file(
    input: installed_tests_template,
    output: 'pw-test-protocol-native.test',
    install_dir: installed_tests_metadir,
    configuration: test_conf
  )
endif

pipewire_module_adapter = shared_library('pipewire-module-adapter',
  [ 'module-adapter.c',
    'module-adapter/adapter.c',
    'spa/spa-node.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

pipewire_module_session_manager = shared_library('pipewire-module-session-manager',
  [ 'module-session-manager.c',
    'module-session-manager/client-endpoint/client-endpoint.c',
    'module-session-manager/client-endpoint/endpoint-stream.c',
    'module-session-manager/client-endpoint/endpoint.c',
    'module-session-manager/client-session/client-session.c',
    'module-session-manager/client-session/endpoint-link.c',
    'module-session-manager/client-session/session.c',
    'module-session-manager/endpoint-link.c',
    'module-session-manager/endpoint-stream.c',
    'module-session-manager/endpoint.c',
    'module-session-manager/protocol-native.c',
    'module-session-manager/proxy-session-manager.c',
    'module-session-manager/session.c',
  ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

if avahi_dep.found()
pipewire_module_zeroconf_discover = shared_library('pipewire-module-zeroconf-discover',
  [ 'module-zeroconf-discover.c',
    'module-zeroconf-discover/avahi-poll.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, avahi_dep],
)
endif

if roc_lib.found()
pipewire_module_roc_sink = shared_library('pipewire-module-roc-sink',
  [ 'module-roc-sink.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, roc_lib],
)

pipewire_module_roc_source = shared_library('pipewire-module-roc-source',
  [ 'module-roc-source.c' ],
  include_directories : [configinc, spa_inc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, roc_lib],
)
endif
