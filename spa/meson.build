#project('spa', 'c')

#cc = meson.get_compiler('c')
#dl_lib = cc.find_library('dl', required : false)
#pthread_lib = dependencies('threads')
#mathlib = cc.find_library('m', required : false)

spa_dep = declare_dependency(
  include_directories : [
    include_directories('include'),
  ],
  dependencies : [atomic_dep],
  version : spaversion,
  variables : {
    'plugindir' : meson.current_build_dir() / 'plugins',
    'datadir' : meson.current_source_dir() / 'plugins',
  },
)

meson.override_dependency('lib@0@'.format(spa_name), spa_dep)

pkgconfig.generate(filebase : 'lib@0@'.format(spa_name),
  name : 'libspa',
  subdirs : spa_name,
  description : 'Simple Plugin API',
  version : spaversion,
  extra_cflags : '-D_REENTRANT',
  variables : ['plugindir=${libdir}/@0@'.format(spa_name)],
  uninstalled_variables : ['plugindir=${prefix}/spa/plugins'],
)

subdir('include')

if get_option('spa-plugins').allowed()
  udevrulesdir = get_option('udevrulesdir')
  if udevrulesdir == ''
    # absolute path, otherwise meson prepends the prefix
    udevrulesdir = '/lib/udev/rules.d'
  endif

  # plugin-specific dependencies
  alsa_dep = dependency('alsa', required: get_option('alsa'))
  summary({'ALSA': alsa_dep.found()}, bool_yn: true, section: 'Backend')

  bluez_dep = dependency('bluez', version : '>= 4.101', required: get_option('bluez5'))
  gio_dep = dependency('gio-2.0', required : get_option('bluez5'))
  gio_unix_dep = dependency('gio-unix-2.0', required : get_option('bluez5'))
  bluez_glib2_dep = dependency('glib-2.0', required : get_option('bluez5'))
  sbc_dep = dependency('sbc', required: get_option('bluez5'))
  summary({'SBC': sbc_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
  bluez5_deps = [ mathlib, dbus_dep, sbc_dep, bluez_dep, bluez_glib2_dep, gio_dep, gio_unix_dep ]
  bluez_deps_found = get_option('bluez5').allowed()
  foreach dep: bluez5_deps
      if get_option('bluez5').enabled() and not dep.found()
        error('bluez5 enabled, but dependency not found: ' + dep.name())
      endif
      bluez_deps_found = bluez_deps_found and dep.found()
  endforeach
  summary({'Bluetooth audio': bluez_deps_found}, bool_yn: true, section: 'Backend')
  if bluez_deps_found
    ldac_dep = dependency('ldacBT-enc', required : get_option('bluez5-codec-ldac'))
    summary({'LDAC': ldac_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    ldac_abr_dep = dependency('ldacBT-abr', required : get_option('bluez5-codec-ldac'))
    summary({'LDAC ABR': ldac_abr_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    aptx_dep = dependency('libfreeaptx', required : get_option('bluez5-codec-aptx'))
    summary({'aptX': aptx_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    fdk_aac_dep = dependency('fdk-aac', required : get_option('bluez5-codec-aac'))
    summary({'AAC': fdk_aac_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    lc3plus_dep = dependency('lc3plus', required : false)
    if not lc3plus_dep.found()
      lc3plus_lc3plus_h_dep = cc.find_library('LC3plus', has_headers: ['lc3plus.h'], required : get_option('bluez5-codec-lc3plus'))
      if lc3plus_lc3plus_h_dep.found()
        lc3plus_dep = declare_dependency(compile_args : '-DHAVE_LC3PLUS_H', dependencies : [ lc3plus_lc3plus_h_dep ])
      endif
    endif
    summary({'LC3plus': lc3plus_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    if get_option('bluez5-codec-opus').enabled() and not opus_dep.found()
        error('bluez5-codec-opus enabled, but opus dependency not found')
    endif
    summary({'Opus': opus_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    lc3_dep = dependency('lc3', required : get_option('bluez5-codec-lc3'))
    summary({'LC3': lc3_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    if get_option('bluez5-backend-hsp-native').allowed() or get_option('bluez5-backend-hfp-native').allowed()
      mm_dep = dependency('ModemManager', version : '>= 1.10.0', required : get_option('bluez5-backend-native-mm'))
      summary({'ModemManager': mm_dep.found()}, bool_yn: true, section: 'Bluetooth backends')
    endif
  endif
  jack_dep = dependency('jack', version : '>= 1.9.10', required: get_option('jack'))
  summary({'JACK2': jack_dep.found()}, bool_yn: true, section: 'Backend')
  vulkan_dep = dependency('vulkan', disabler : true, version : '>= 1.1.69', required: get_option('vulkan'))
  vulkan_headers = cc.has_header('vulkan/vulkan.h', dependencies : vulkan_dep)
  #summary({'Vulkan': vulkan_headers}, bool_yn: true, section: 'Misc dependencies')

  libcamera_dep = dependency('libcamera', required: get_option('libcamera'))
  summary({'libcamera': libcamera_dep.found()}, bool_yn: true, section: 'Backend')
  cdata.set('HAVE_LIBCAMERA_SYSTEM_DEVICES', libcamera_dep.version().version_compare('>= 0.1.0'))

  compress_offload_option = get_option('compress-offload')
  summary({'Compress-Offload': compress_offload_option.allowed()}, bool_yn: true, section: 'Backend')
  cdata.set('HAVE_ALSA_COMPRESS_OFFLOAD', compress_offload_option.allowed())

  # common dependencies
  libudev_dep = dependency('libudev', required: alsa_dep.found() or get_option('udev').enabled() or get_option('v4l2').enabled())
  summary({'Udev': libudev_dep.found()}, bool_yn: true, section: 'Backend')

  subdir('plugins')
endif

subdir('tools')
subdir('tests')
if get_option('examples').allowed()
  subdir('examples')
endif
