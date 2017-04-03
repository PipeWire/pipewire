#project('spa', 'c')

alsa_dep = dependency('alsa')
v4l2_dep = dependency('libv4l2')
xv_dep = dependency('x11')
sdl_dep = dependency('sdl2')
avcodec_dep = dependency('libavcodec')
avformat_dep = dependency('libavformat')
avfilter_dep = dependency('libavfilter')
libva_dep = dependency('libva')
libudev_dep = dependency('libudev')
threads_dep = dependency('threads')

cc = meson.get_compiler('c')

dl_lib = cc.find_library('dl', required : true)
pthread_lib = cc.find_library('pthread', required : true)
libm = cc.find_library('m', required : true)

spa_inc = include_directories('include')
spa_libinc = include_directories('.')

subdir('include')
subdir('lib')
subdir('plugins')
subdir('tools')
subdir('tests')
