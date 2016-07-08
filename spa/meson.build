project('spa', 'c')

alsa_dep = dependency('alsa')
v4l2_dep = dependency('libv4l2')
xv_dep = dependency('x11')
sdl_dep = dependency('sdl2')

cc = meson.get_compiler('c')

dl_lib = cc.find_library('dl', required : true)
pthread_lib = cc.find_library('pthread', required : true)

inc = include_directories('include')

subdir('include')
subdir('lib')
subdir('plugins')
subdir('tools')
subdir('tests')
