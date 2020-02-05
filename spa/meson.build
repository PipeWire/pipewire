#project('spa', 'c')

#cc = meson.get_compiler('c')
#dl_lib = cc.find_library('dl', required : false)
#pthread_lib = dependencies('threads')
#mathlib = cc.find_library('m', required : false)

spa_inc = include_directories('include')

subdir('include')

if get_option('spa-plugins')
  # common dependencies
  if get_option('alsa') or get_option('v4l2')
    libudev_dep = dependency('libudev')
  endif

  # plugin-specific dependencies
  if get_option('alsa')
    alsa_dep = dependency('alsa')
  endif
  if get_option('bluez5')
    bluez_dep = dependency('bluez', version : '>= 4.101')
    sbc_dep = dependency('sbc')
  endif
  if get_option('ffmpeg')
    avcodec_dep = dependency('libavcodec')
    avformat_dep = dependency('libavformat')
  endif
  if get_option('jack')
    jack_dep = dependency('jack', version : '>= 1.9.10')
  endif
  if get_option('vulkan')
    vulkan_dep = dependency('vulkan')
  endif

  subdir('plugins')
endif

subdir('tools')
subdir('tests')
if get_option('examples')
  subdir('examples')
endif

pkgconfig.generate(filebase : 'lib@0@'.format(spa_name),
  name : 'libspa',
  subdirs : spa_name,
  description : 'Simple Plugin API',
  version : spaversion,
  extra_cflags : '-D_REENTRANT')
