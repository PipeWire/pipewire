bluez5_deps = [ mathlib, dbus_dep, sbc_dep, bluez_dep ]
foreach dep: bluez5_deps
  if not dep.found()
    subdir_done()
  endif
endforeach

cdata.set('HAVE_BLUEZ_5_BACKEND_NATIVE',
          get_option('bluez5-backend-hsp-native').allowed() or
          get_option('bluez5-backend-hfp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HSP_NATIVE', get_option('bluez5-backend-hsp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HFP_NATIVE', get_option('bluez5-backend-hfp-native').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_OFONO', get_option('bluez5-backend-ofono').allowed())
cdata.set('HAVE_BLUEZ_5_BACKEND_HSPHFPD', get_option('bluez5-backend-hsphfpd').allowed())
cdata.set('HAVE_BLUEZ_5_HCI', dependency('bluez', version: '< 6', required: false).found())

bluez5_sources = [
  'plugin.c',
  'codec-loader.c',
  'a2dp-codecs.c',
  'a2dp-sink.c',
  'a2dp-source.c',
  'sco-sink.c',
  'sco-source.c',
  'sco-io.c',
  'quirks.c',
  'player.c',
  'bluez5-device.c',
  'bluez5-dbus.c',
  'hci.c'
]

bluez5_data = ['bluez-hardware.conf']

install_data(bluez5_data, install_dir : spa_datadir / 'bluez5')

if get_option('bluez5-backend-hsp-native').allowed() or get_option('bluez5-backend-hfp-native').allowed()
  if libusb_dep.found()
    bluez5_deps += libusb_dep
  endif
  bluez5_sources += ['backend-native.c']
endif

if get_option('bluez5-backend-ofono').allowed()
  bluez5_sources += ['backend-ofono.c']
endif

if get_option('bluez5-backend-hsphfpd').allowed()
  bluez5_sources += ['backend-hsphfpd.c']
endif

bluez5lib = shared_library('spa-bluez5',
  bluez5_sources,
  include_directories : [ configinc ],
  dependencies : [ spa_dep, bluez5_deps ],
  install : true,
  install_dir : spa_plugindir / 'bluez5')

codec_args = [ '-DCODEC_PLUGIN' ]

bluez_codec_sbc = shared_library('spa-codec-bluez5-sbc',
  [ 'a2dp-codec-sbc.c', 'a2dp-codecs.c' ],
  include_directories : [ configinc ],
  c_args : codec_args,
  dependencies : [ spa_dep, sbc_dep ],
  install : true,
  install_dir : spa_plugindir / 'bluez5')

bluez_codec_faststream = shared_library('spa-codec-bluez5-faststream',
  [ 'a2dp-codec-faststream.c', 'a2dp-codecs.c' ],
  include_directories : [ configinc ],
  c_args : codec_args,
  dependencies : [ spa_dep, sbc_dep ],
  install : true,
  install_dir : spa_plugindir / 'bluez5')

if fdk_aac_dep.found()
  bluez_codec_aac = shared_library('spa-codec-bluez5-aac',
    [ 'a2dp-codec-aac.c', 'a2dp-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, fdk_aac_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if aptx_dep.found()
  bluez_codec_aptx = shared_library('spa-codec-bluez5-aptx',
    [ 'a2dp-codec-aptx.c', 'a2dp-codecs.c' ],
    include_directories : [ configinc ],
    c_args : codec_args,
    dependencies : [ spa_dep, aptx_dep, sbc_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif

if ldac_dep.found()
  ldac_args = codec_args
  ldac_dep = [ ldac_dep ]
  if ldac_abr_dep.found()
    ldac_args += [ '-DENABLE_LDAC_ABR' ]
    ldac_dep += ldac_abr_dep
  endif
  bluez_codec_ldac = shared_library('spa-codec-bluez5-ldac',
    [ 'a2dp-codec-ldac.c', 'a2dp-codecs.c' ],
    include_directories : [ configinc ],
    c_args : ldac_args,
    dependencies : [ spa_dep, ldac_dep ],
    install : true,
    install_dir : spa_plugindir / 'bluez5')
endif
