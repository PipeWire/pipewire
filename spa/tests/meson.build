test_apps = [
	'test-buffer',
	'test-node',
	'test-pod',
	'test-utils',
]

foreach a : test_apps
  test('spa-' + a,
	executable('spa-' + a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach

if have_cpp
test_cpp = executable('spa-test-cpp', 'test-cpp.cpp',
                        include_directories : [spa_inc ],
                        dependencies : [],
                        install : false)
test('spa-test-cpp', test_cpp)
endif

benchmark_apps = [
	'stress-ringbuffer',
	'benchmark-pod',
	'benchmark-dict',
]

foreach a : benchmark_apps
  benchmark('spa-' + a,
	executable('spa-' + a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach
