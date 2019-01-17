test_apps = [
	'test-buffer',
	'test-pod',
	'test-utils',
	'stress-ringbuffer',
]

foreach a : test_apps
  test(a,
	executable(a, a + '.c',
		dependencies : [dl_lib, pthread_lib],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach

if have_cpp
test_cpp = executable('test-cpp', 'test-cpp.cpp',
                        include_directories : [spa_inc ],
                        dependencies : [],
                        install : false)
test('test-cpp', test_cpp)
endif
