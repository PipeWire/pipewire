project('spa', 'c')

alsa_dep = dependency('alsa')
v4l2_dep = dependency('libv4l2')
dl_lib = find_library('dl', required : true)

inc = include_directories('include')

subdir('include')
subdir('lib')
subdir('plugins')
subdir('tools')
subdir('tests')
