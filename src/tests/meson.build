test_apps = [
	'test-array',
	'test-client',
	'test-core',
	'test-remote'
]

foreach a : test_apps
  test(a, executable(a, a + '.c',
                        dependencies : [pipewire_dep],
			c_args : [ '-D_GNU_SOURCE' ],
                        install : false),
			env : ['SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root())])
endforeach


if have_cpp
test_cpp = executable('test-cpp', 'test-cpp.cpp',
                        dependencies : [pipewire_dep],
                        install : false)
test('test-cpp', test_cpp)
endif
