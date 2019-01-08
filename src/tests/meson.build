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
                        install : false))
endforeach


if have_cpp
test_cpp = executable('test-cpp', 'test-cpp.cpp',
                        dependencies : [pipewire_dep],
                        install : false)
test('test-cpp', test_cpp)
endif
