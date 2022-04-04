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
  summary({'Bluetooth audio': bluez_dep.found()}, bool_yn: true, section: 'Backend')
  if bluez_dep.found()
    sbc_dep = dependency('sbc', required: get_option('bluez5'))
    summary({'SBC': sbc_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    ldac_dep = dependency('ldacBT-enc', required : get_option('bluez5-codec-ldac'))
    summary({'LDAC': ldac_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    ldac_abr_dep = dependency('ldacBT-abr', required : get_option('bluez5-codec-ldac'))
    summary({'LDAC ABR': ldac_abr_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    aptx_dep = dependency('libfreeaptx', required : get_option('bluez5-codec-aptx'))
    summary({'aptX': aptx_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
    fdk_aac_dep = dependency('fdk-aac', required : get_option('bluez5-codec-aac'))
    summary({'AAC': fdk_aac_dep.found()}, bool_yn: true, section: 'Bluetooth audio codecs')
  endif
  avcodec_dep = dependency('libavcodec', required: get_option('ffmpeg'))
  jack_dep = dependency('jack', version : '>= 1.9.10', required: get_option('jack'))
  summary({'JACK2': jack_dep.found()}, bool_yn: true, section: 'Backend')
  vulkan_dep = dependency('vulkan', disabler : true, version : '>= 1.1.69', required: get_option('vulkan'))
  vulkan_headers = cc.has_header('vulkan/vulkan.h', dependencies : vulkan_dep)
  #summary({'Vulkan': vulkan_headers}, bool_yn: true, section: 'Misc dependencies')

  libcamera_dep = dependency('libcamera', required: get_option('libcamera'))
  summary({'libcamera': libcamera_dep.found()}, bool_yn: true, section: 'Backend')

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
