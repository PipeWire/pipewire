gnome = import('gnome')

cdata.set('HAVE_BLUEZ_5_BACKEND_NATIVE',
          get_option('bluez5-backend-hsp-native').allowed() or
          get_option('bluez5-backend-hfp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HSP_NATIVE', get_option('bluez5-backend-hsp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HFP_NATIVE', get_option('bluez5-backend-hfp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_NATIVE_MM', get_option('bluez5-backend-native-mm').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_OFONO', get_option('bluez5-backend-ofono').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HSPHFPD', get_option('bluez5-backend-hsphfpd').allowed())
cdata.set('HAVE_BLUEZ_5_HCI', dependency('bluez', version: '< 6', required: false).found())

bluez5_sources = [
  'plugin.c',
  'codec-loader.c',
  'media-codecs.c',
  'media-sink.c',
  'media-source.c',
  'sco-sink.c',
  'sco-source.c',
  'sco-io.c',
  'iso-io.c',
  'quirks.c',
  'player.c',
  'bluez5-device.c',
  'bluez5-dbus.c',
  'hci.c',
  'dbus-monitor.c',
  'midi-enum.c',
  'midi-parser.c',
  'midi-node.c',
  'midi-server.c',
]

bluez5_interface_src = gnome.gdbus_codegen('bluez5-interface-gen',
  sources: 'org.bluez.xml',
  interface_prefix : 'org.bluez.',
  object_manager: true,
  namespace : 'Bluez5',
  annotations : [
    ['org.bluez.GattCharacteristic1.AcquireNotify()', 'org.gtk.GDBus.C.UnixFD', 'true'],
    ['org.bluez.GattCharacteristic1.AcquireWrite()', 'org.gtk.GDBus.C.UnixFD', 'true'],
  ]
)
bluez5_sources += [ bluez5_interface_src ]

bluez5_data = ['bluez-hardware.conf']

install_data(bluez5_data, install_dir : spa_datadir / 'bluez5')

if get_option('bluez5-backend-hsp-native').allowed() or get_option('bluez5-backend-hfp-native').allowed()
  if libusb_dep.found()
    bluez5_deps += libusb_dep
  endif
  if mm_dep.found()
    bluez5_deps += mm_dep
    bluez5_sources += ['modemmanager.c']
  endif
  bluez5_sources += ['backend-native.c', 'upower.c']
endif

if get_option('bluez5-backend-ofono').allowed()
  bluez5_sources += ['backend-ofono.c']
endif

if get_option('bluez5-backend-hsphfpd').allowed()
  bluez5_sources += ['backend-hsphfpd.c']
endif

# The library uses GObject, and cannot be unloaded
bluez5_link_args = [ '-Wl,-z', '-Wl,nodelete' ]

bluez5lib = shared_library('spa-bluez5',
  bluez5_sources,
  include_directories : [ configinc ],
  dependencies : [ spa_dep, bluez5_deps ],
  link_args : bluez5_link_args,
  install : true,
  install_dir : spa_plugindir / 'bluez5')

codec_args = [ '-DCODEC_PLUGIN' ]

bluez_codec_sbc = shared_library('spa-codec-bluez5-sbc',
  [ 'a2dp-codec-sbc.c', 'media-codecs.c' ],
  include_directories : [ configinc ],
  c_args : codec_args,
  dependencies : [ spa_dep, sbc_dep ],
  install : true,
  install_dir : spa_plugindir / 'bluez5')

bluez_codec_faststream = shared_library('spa-codec-bluez5-faststream',
  [ 'a2dp-codec-faststream.c', 'media-codecs.c' ],
  include_directories : [ configinc ],
  c_args : codec_args,
  dependencies : [ spa_dep, sbc_dep ],
  install : true,
  install_dir : spa_plugindir / 'bluez5')

if fdk_aac_dep.found()
  bluez_codec_aac = shared_library('spa-codec-bluez5-aac',
    [ 'a2dp-codec-aac.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, fdk_aac_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if aptx_dep.found()
  bluez_codec_aptx = shared_library('spa-codec-bluez5-aptx',
    [ 'a2dp-codec-aptx.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, aptx_dep, sbc_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if ldac_dep.found()
  ldac_args = codec_args
  if ldac_abr_dep.found()
    ldac_args += [ '-DENABLE_LDAC_ABR' ]
  endif
  bluez_codec_ldac = shared_library('spa-codec-bluez5-ldac',
    [ 'a2dp-codec-ldac.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : ldac_args,
    dependencies : [ spa_dep, ldac_dep, ldac_abr_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if get_option('bluez5-codec-lc3plus').allowed() and lc3plus_dep.found()
  bluez_codec_lc3plus = shared_library('spa-codec-bluez5-lc3plus',
    [ 'a2dp-codec-lc3plus.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, lc3plus_dep, mathlib ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if get_option('bluez5-codec-opus').allowed() and opus_dep.found()
  opus_args = codec_args
  bluez_codec_opus = shared_library('spa-codec-bluez5-opus',
    [ 'a2dp-codec-opus.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : opus_args,
    dependencies : [ spa_dep, opus_dep, mathlib ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if get_option('bluez5-codec-lc3').allowed() and lc3_dep.found()
  bluez_codec_lc3 = shared_library('spa-codec-bluez5-lc3',
    [ 'bap-codec-lc3.c', 'media-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, lc3_dep, mathlib ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

test_apps = [
  'test-midi',
]
bluez5_test_lib = static_library('bluez5_test_lib',
  [ 'midi-parser.c' ],
  include_directories : [ configinc ],
  dependencies : [ spa_dep, bluez5_deps ],
  install : false
)

foreach a : test_apps
  test(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib, bluez5_deps ],
      include_directories : [ configinc ],
      link_with : [ bluez5_test_lib ],
      install_rpath : spa_plugindir / 'bluez5',
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'bluez5'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'bluez5' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'bluez5',
        configuration: test_conf
        )
  endif
endforeach
