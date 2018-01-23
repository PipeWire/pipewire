#project('spa', 'c')

alsa_dep = dependency('alsa')
v4l2_dep = dependency('libv4l2')
x11_dep = dependency('x11', required : false)
sdl_dep = dependency('sdl2', required : false)
avcodec_dep = dependency('libavcodec', required : false)
avformat_dep = dependency('libavformat', required : false)
avfilter_dep = dependency('libavfilter', required : false)
libva_dep = dependency('libva', required : false)
sbc_dep = dependency('sbc', required : false)
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
