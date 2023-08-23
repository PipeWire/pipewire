subdir('module-rt')
subdir('spa')

# The list of "main" source files for modules, the ones that have the
# doxygen documentation
module_sources = [
  'module-access.c',
  'module-adapter.c',
  'module-avb.c',
  'module-client-device.c',
  'module-client-node.c',
  'module-combine-stream.c',
  'module-echo-cancel.c',
  'module-example-filter.c',
  'module-example-sink.c',
  'module-example-source.c',
  'module-fallback-sink.c',
  'module-ffado-driver.c',
  'module-filter-chain.c',
  'module-jack-tunnel.c',
  'module-jackdbus-detect.c',
  'module-link-factory.c',
  'module-loopback.c',
  'module-metadata.c',
  'module-netjack2-driver.c',
  'module-netjack2-manager.c',
  'module-pipe-tunnel.c',
  'module-portal.c',
  'module-profiler.c',
  'module-protocol-native.c',
  'module-protocol-pulse.c',
  'module-protocol-simple.c',
  'module-pulse-tunnel.c',
  'module-rt.c',
  'module-raop-discover.c',
  'module-raop-sink.c',
  'module-rtp-sap.c',
  'module-rtp-session.c',
  'module-rtp-source.c',
  'module-rtp-sink.c',
  'module-vban-recv.c',
  'module-vban-send.c',
  'module-session-manager.c',
  'module-zeroconf-discover.c',
  'module-roc-source.c',
  'module-roc-sink.c',
  'module-x11-bell.c',
]

pipewire_module_access_deps = [spa_dep, mathlib, dl_lib, pipewire_dep]
if flatpak_support
  pipewire_module_access_deps += glib2_dep
endif

pipewire_module_access = shared_library('pipewire-module-access', [ 'module-access.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_access_deps
)

pipewire_module_loopback = shared_library('pipewire-module-loopback',
  [ 'module-loopback.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

simd_cargs = []
simd_dependencies = []

if have_sse
  filter_chain_sse = static_library('filter_chain_sse',
    ['module-filter-chain/pffft.c',
     'module-filter-chain/dsp-ops-sse.c' ],
    c_args : [sse_args, '-O3', '-DHAVE_SSE'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_SSE']
  simd_dependencies += filter_chain_sse
endif
if have_avx
  filter_chain_avx = static_library('filter_chain_avx',
    ['module-filter-chain/dsp-ops-avx.c' ],
    c_args : [avx_args, fma_args,'-O3', '-DHAVE_AVX'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_AVX']
  simd_dependencies += filter_chain_avx
endif
if have_neon
  filter_chain_neon = static_library('filter_chain_neon',
    ['module-filter-chain/pffft.c' ],
    c_args : [neon_args, '-O3', '-DHAVE_NEON'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_NEON']
  simd_dependencies += filter_chain_neon
endif

filter_chain_c = static_library('filter_chain_c',
  ['module-filter-chain/pffft.c',
   'module-filter-chain/dsp-ops.c',
   'module-filter-chain/dsp-ops-c.c' ],
  c_args : [simd_cargs, '-O3', '-DPFFFT_SIMD_DISABLE'],
  dependencies : [ spa_dep ],
  install : false
)
simd_dependencies += filter_chain_c

filter_chain_sources = [
  'module-filter-chain.c',
  'module-filter-chain/biquad.c',
  'module-filter-chain/ladspa_plugin.c',
  'module-filter-chain/builtin_plugin.c',
  'module-filter-chain/convolver.c'
]
filter_chain_dependencies = [
  mathlib, dl_lib, pipewire_dep, sndfile_dep, audioconvert_dep
]

pipewire_module_filter_chain = shared_library('pipewire-module-filter-chain',
  filter_chain_sources,
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  link_with : simd_dependencies,
  dependencies : filter_chain_dependencies,
)

if libmysofa_dep.found()
pipewire_module_filter_chain_sofa = shared_library('pipewire-module-filter-chain-sofa',
  [ 'module-filter-chain/sofa_plugin.c',
    'module-filter-chain/convolver.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  link_with : simd_dependencies,
  dependencies : [ filter_chain_dependencies, libmysofa_dep ]
)
endif

if lilv_lib.found()
pipewire_module_filter_chain_lv2 = shared_library('pipewire-module-filter-chain-lv2',
  [ 'module-filter-chain/lv2_plugin.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [ filter_chain_dependencies, lilv_lib ]
)
endif


pipewire_module_combine_stream = shared_library('pipewire-module-combine-stream',
  [ 'module-combine-stream.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, dl_lib, pipewire_dep],
)

pipewire_module_echo_cancel = shared_library('pipewire-module-echo-cancel',
  [ 'module-echo-cancel.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, audioconvert_dep],
)

build_module_jack_tunnel = jack_dep.found()
if build_module_jack_tunnel
  pipewire_module_jack_tunnel = shared_library('pipewire-module-jack-tunnel',
    [ 'module-jack-tunnel.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, pipewire_dep],
  )
  build_module_jackdbus_detect = dbus_dep.found()
  if build_module_jackdbus_detect
    pipewire_module_jack_tunnel = shared_library('pipewire-module-jackdbus-detect',
      [ 'module-jackdbus-detect.c' ],
      include_directories : [configinc],
      install : true,
      install_dir : modules_install_dir,
      install_rpath: modules_install_dir,
      dependencies : [mathlib, dl_lib, pipewire_dep, dbus_dep],
    )
  endif
endif

summary({'jack-tunnel': build_module_jack_tunnel}, bool_yn: true, section: 'Optional Modules')

build_module_ffado_driver = libffado_dep.found()
if build_module_ffado_driver
  pipewire_module_jack_tunnel = shared_library('pipewire-module-ffado-driver',
    [ 'module-ffado-driver.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, pipewire_dep, libffado_dep],
  )
endif

summary({'ffado-driver': build_module_ffado_driver}, bool_yn: true, section: 'Optional Modules')

opus_custom_h = cc.has_header('opus/opus_custom.h', dependencies: opus_dep)
# One would imagine that opus_dep is a requirement but for some reason it's not, so we need to manually check that
if opus_dep.found() and opus_custom_h
  opus_custom_dep = declare_dependency(compile_args: ['-DHAVE_OPUS_CUSTOM'], dependencies: opus_dep)
else
  opus_custom_dep = dependency('', required: false)
endif
summary({'Opus with custom modes for NetJack2': opus_custom_dep}, bool_yn: true, section: 'Streaming between daemons')

pipewire_module_netjack2_driver = shared_library('pipewire-module-netjack2-driver',
  [ 'module-netjack2-driver.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep, opus_custom_dep],
)

pipewire_module_netjack2_manager = shared_library('pipewire-module-netjack2-manager',
  [ 'module-netjack2-manager.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep, opus_custom_dep],
)

pipewire_module_profiler = shared_library('pipewire-module-profiler',
  [ 'module-profiler.c',
    'module-profiler/protocol-native.c', ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_rt = shared_library('pipewire-module-rt', [ 'module-rt.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
)

build_module_rtkit = dbus_dep.found() and (get_option('legacy-rtkit') == true)
if build_module_rtkit
  pipewire_module_rtkit = shared_library('pipewire-module-rtkit', [ 'module-rt.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
  )
endif
summary({'rt': '@0@ RTKit'.format(build_module_rtkit ? 'with' : 'without')}, section: 'Optional Modules')

build_module_portal = dbus_dep.found()
if build_module_portal
  pipewire_module_portal = shared_library('pipewire-module-portal', [ 'module-portal.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
  )
endif
summary({'portal': build_module_portal}, bool_yn: true, section: 'Optional Modules')

pipewire_module_client_device = shared_library('pipewire-module-client-device',
  [ 'module-client-device.c',
    'module-client-device/resource-device.c',
    'module-client-device/proxy-device.c',
    'module-client-device/protocol-native.c', ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_link_factory = shared_library('pipewire-module-link-factory',
  [ 'module-link-factory.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
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
    'module-protocol-native/protocol-footer.c',
    'module-protocol-native/connection.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_deps,
)

pipewire_module_protocol_pulse_deps = pipewire_module_protocol_deps

pipewire_module_protocol_pulse_sources = [
  'module-protocol-pulse.c',
  'module-protocol-pulse/client.c',
  'module-protocol-pulse/collect.c',
  'module-protocol-pulse/cmd.c',
  'module-protocol-pulse/extension.c',
  'module-protocol-pulse/extensions/ext-device-manager.c',
  'module-protocol-pulse/extensions/ext-device-restore.c',
  'module-protocol-pulse/extensions/ext-stream-restore.c',
  'module-protocol-pulse/format.c',
  'module-protocol-pulse/manager.c',
  'module-protocol-pulse/message.c',
  'module-protocol-pulse/message-handler.c',
  'module-protocol-pulse/module.c',
  'module-protocol-pulse/operation.c',
  'module-protocol-pulse/pending-sample.c',
  'module-protocol-pulse/pulse-server.c',
  'module-protocol-pulse/quirks.c',
  'module-protocol-pulse/remap.c',
  'module-protocol-pulse/reply.c',
  'module-protocol-pulse/sample.c',
  'module-protocol-pulse/sample-play.c',
  'module-protocol-pulse/server.c',
  'module-protocol-pulse/stream.c',
  'module-protocol-pulse/utils.c',
  'module-protocol-pulse/volume.c',
  'module-protocol-pulse/modules/module-alsa-sink.c',
  'module-protocol-pulse/modules/module-alsa-source.c',
  'module-protocol-pulse/modules/module-always-sink.c',
  'module-protocol-pulse/modules/module-combine-sink.c',
  'module-protocol-pulse/modules/module-echo-cancel.c',
  'module-protocol-pulse/modules/module-jackdbus-detect.c',
  'module-protocol-pulse/modules/module-ladspa-sink.c',
  'module-protocol-pulse/modules/module-ladspa-source.c',
  'module-protocol-pulse/modules/module-loopback.c',
  'module-protocol-pulse/modules/module-native-protocol-tcp.c',
  'module-protocol-pulse/modules/module-null-sink.c',
  'module-protocol-pulse/modules/module-pipe-source.c',
  'module-protocol-pulse/modules/module-pipe-sink.c',
  'module-protocol-pulse/modules/module-raop-discover.c',
  'module-protocol-pulse/modules/module-remap-sink.c',
  'module-protocol-pulse/modules/module-remap-source.c',
  'module-protocol-pulse/modules/module-roc-sink.c',
  'module-protocol-pulse/modules/module-roc-sink-input.c',
  'module-protocol-pulse/modules/module-roc-source.c',
  'module-protocol-pulse/modules/module-rtp-recv.c',
  'module-protocol-pulse/modules/module-rtp-send.c',
  'module-protocol-pulse/modules/module-simple-protocol-tcp.c',
  'module-protocol-pulse/modules/module-switch-on-connect.c',
  'module-protocol-pulse/modules/module-tunnel-sink.c',
  'module-protocol-pulse/modules/module-tunnel-source.c',
  'module-protocol-pulse/modules/module-virtual-sink.c',
  'module-protocol-pulse/modules/module-virtual-source.c',
  'module-protocol-pulse/modules/module-x11-bell.c',
  'module-protocol-pulse/modules/module-zeroconf-discover.c',
]

if dbus_dep.found()
  pipewire_module_protocol_pulse_sources += [
    'module-protocol-pulse/dbus-name.c',
  ]
  pipewire_module_protocol_pulse_deps += dbus_dep
endif

if avahi_dep.found()
  pipewire_module_protocol_pulse_sources += [
    'module-protocol-pulse/modules/module-zeroconf-publish.c',
    'module-zeroconf-discover/avahi-poll.c',
  ]
  pipewire_module_protocol_pulse_deps += avahi_dep
  cdata.set('HAVE_AVAHI', true)
endif

if gio_dep.found()
  pipewire_module_protocol_pulse_sources += [
    'module-protocol-pulse/modules/module-gsettings.c',
  ]
  pipewire_module_protocol_pulse_deps += gio_dep
  cdata.set('HAVE_GIO', true)
endif

if flatpak_support
  pipewire_module_protocol_pulse_deps += glib2_dep
endif

pipewire_module_protocol_pulse = shared_library('pipewire-module-protocol-pulse',
  pipewire_module_protocol_pulse_sources,
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_pulse_deps,
)

build_module_pulse_tunnel = pulseaudio_dep.found()
  if build_module_pulse_tunnel
    pipewire_module_pulse_tunnel = shared_library('pipewire-module-pulse-tunnel',
    [ 'module-pulse-tunnel.c',
      'module-protocol-pulse/format.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, pipewire_dep, pulseaudio_dep],
  )
endif
summary({'pulse-tunnel': build_module_pulse_tunnel}, bool_yn: true, section: 'Optional Modules')

pipewire_module_pipe_tunnel = shared_library('pipewire-module-pipe-tunnel',
  [ 'module-pipe-tunnel.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_protocol_simple = shared_library('pipewire-module-protocol-simple',
  [ 'module-protocol-simple.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : pipewire_module_protocol_deps,
)

pipewire_module_example_filter = shared_library('pipewire-module-example-filter',
  [ 'module-example-filter.c' ],
  include_directories : [configinc],
  install : false,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_example_sink = shared_library('pipewire-module-example-sink',
  [ 'module-example-sink.c' ],
  include_directories : [configinc],
  install : false,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_example_sink = shared_library('pipewire-module-example-source',
  [ 'module-example-source.c' ],
  include_directories : [configinc],
  install : false,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
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
  include_directories : [configinc],
  link_with : pipewire_module_protocol_native,
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_metadata = shared_library('pipewire-module-metadata',
  [ 'module-metadata.c',
    'module-metadata/proxy-metadata.c',
    'module-metadata/metadata.c',
    'module-metadata/protocol-native.c'],
  include_directories : [configinc],
  link_with : pipewire_module_protocol_native,
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

test('pw-test-protocol-native',
  executable('pw-test-protocol-native',
    [ 'module-protocol-native/test-connection.c',
      'module-protocol-native/connection.c' ],
    c_args : libpipewire_c_args,
    include_directories : [configinc ],
    dependencies : [spa_dep, pipewire_dep],
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir,
  ),
  env : [
    'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
    'PIPEWIRE_CONFIG_DIR=@0@'.format(pipewire_dep.get_variable('confdatadir')),
    'PIPEWIRE_MODULE_DIR=@0@'.format(pipewire_dep.get_variable('moduledir')),
  ]
)

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
  include_directories : [configinc],
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
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

build_module_zeroconf_discover = avahi_dep.found()
if build_module_zeroconf_discover
  pipewire_module_zeroconf_discover = shared_library('pipewire-module-zeroconf-discover',
    [ 'module-zeroconf-discover.c',
      'module-protocol-pulse/format.c',
      'module-zeroconf-discover/avahi-poll.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, avahi_dep],
  )
endif
summary({'zeroconf-discover': build_module_zeroconf_discover}, bool_yn: true, section: 'Optional Modules')

build_module_raop_discover = avahi_dep.found()
if build_module_raop_discover
  pipewire_module_raop_discover = shared_library('pipewire-module-raop-discover',
    [ 'module-raop-discover.c',
      'module-zeroconf-discover/avahi-poll.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, avahi_dep],
  )
endif
summary({'raop-discover (needs Avahi)': build_module_raop_discover}, bool_yn: true, section: 'Optional Modules')

build_module_raop = openssl_lib.found()
if build_module_raop
  pipewire_module_raop_sink = shared_library('pipewire-module-raop-sink',
    [ 'module-raop-sink.c',
      'module-raop/rtsp-client.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, openssl_lib],
  )
endif
summary({'raop-sink (requires OpenSSL)': build_module_raop}, bool_yn: true, section: 'Optional Modules')

roc_dep = dependency('roc', required: get_option('roc'))
summary({'ROC': roc_dep.found()}, bool_yn: true, section: 'Streaming between daemons')

pipewire_module_rtp_source = shared_library('pipewire-module-rtp-source',
  [ 'module-rtp-source.c',
    'module-rtp/stream.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, opus_dep],
)

pipewire_module_rtp_sink = shared_library('pipewire-module-rtp-sink',
  [ 'module-rtp-sink.c',
    'module-rtp/stream.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, opus_dep],
)

build_module_rtp_session = avahi_dep.found()
if build_module_rtp_session
  pipewire_module_rtp_session = shared_library('pipewire-module-rtp-session',
    [ 'module-rtp/stream.c',
      'module-zeroconf-discover/avahi-poll.c',
     'module-rtp-session.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, avahi_dep, opus_dep],
  )
endif

pipewire_module_rtp_sap = shared_library('pipewire-module-rtp-sap',
  [ 'module-rtp-sap.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

pipewire_module_vban_send = shared_library('pipewire-module-vban-send',
  [ 'module-vban-send.c',
    'module-vban/stream.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

pipewire_module_vban_recv = shared_library('pipewire-module-vban-recv',
  [ 'module-vban-recv.c',
    'module-vban/stream.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

build_module_roc = roc_dep.found()
if build_module_roc
  pipewire_module_roc_sink = shared_library('pipewire-module-roc-sink',
    [ 'module-roc-sink.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, roc_dep],
  )

  pipewire_module_roc_source = shared_library('pipewire-module-roc-source',
    [ 'module-roc-source.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, roc_dep],
  )
endif
summary({'roc-sink': build_module_roc}, bool_yn: true, section: 'Optional Modules')
summary({'roc-source': build_module_roc}, bool_yn: true, section: 'Optional Modules')

build_module_x11_bell = x11_dep.found() and canberra_dep.found()
if build_module_x11_bell
  pipewire_module_x11_bell = shared_library('pipewire-module-x11-bell',
    [ 'module-x11-bell.c' ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep, x11_dep, xfixes_dep, canberra_dep],
  )
endif
summary({'x11-bell': build_module_x11_bell}, bool_yn: true, section: 'Optional Modules')

pipewire_module_fallback_sink = shared_library('pipewire-module-fallback-sink',
  [ 'module-fallback-sink.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  install_rpath: modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

build_module_avb = get_option('avb').require(host_machine.system() == 'linux', error_message: 'AVB support is only available on Linux').allowed()
if build_module_avb
  pipewire_module_avb = shared_library('pipewire-module-avb',
    [ 'module-avb.c',
      'module-avb/avb.c',
      'module-avb/adp.c',
      'module-avb/acmp.c',
      'module-avb/aecp.c',
      'module-avb/aecp-aem.c',
      'module-avb/avdecc.c',
      'module-avb/maap.c',
      'module-avb/mmrp.c',
      'module-avb/mrp.c',
      'module-avb/msrp.c',
      'module-avb/mvrp.c',
      'module-avb/srp.c',
      'module-avb/stream.c'
      ],
    include_directories : [configinc],
    install : true,
    install_dir : modules_install_dir,
    install_rpath: modules_install_dir,
    dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
  )
endif
summary({'avb': build_module_avb}, bool_yn: true, section: 'Optional Modules')
