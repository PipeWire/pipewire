#project('spa', 'c')

alsa_dep = dependency('alsa')
v4l2_dep = dependency('libv4l2')
x11_dep = dependency('x11', required : false)
sdl_dep = dependency('sdl2', required : false)
libva_dep = dependency('libva', required : false)
sbc_dep = dependency('sbc', required : false)
avcodec_dep = dependency('libavcodec', required : false)
avformat_dep = dependency('libavformat', required : false)
avfilter_dep = dependency('libavfilter', required : false)
libudev_dep = dependency('libudev')
threads_dep = dependency('threads')
speexdsp_dep = dependency('speexdsp')
sdl_dep = dependency('sdl2', required : false)

#cc = meson.get_compiler('c')
#dl_lib = cc.find_library('dl', required : false)
#pthread_lib = dependencies('threads')
#mathlib = cc.find_library('m', required : false)

spa_inc = include_directories('include')

subdir('include')

if get_option('spa-plugins')
  subdir('plugins')
endif

subdir('tools')
subdir('tests')

pkgconfig.generate(filebase : 'libspa-@0@'.format(spaversion),
  name : 'libspa',
  description : 'Simple Plugin API',
  version : pipewire_version,
  extra_cflags : '-D_REENTRANT')
