test_apps = [
	'test-array',
	'test-client',
	'test-context',
	'test-endpoint',
	'test-interfaces',
	'test-properties',
	#	'test-remote',
	'test-stream',
	'test-utils'
]

foreach a : test_apps
  test('pw-' + a,
	executable('pw-' + a, a + '.c',
		dependencies : [pipewire_dep],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
		'PIPEWIRE_MODULE_DIR=@0@/src/modules/'.format(meson.build_root())
	])
endforeach


if have_cpp
test_cpp = executable('pw-test-cpp', 'test-cpp.cpp',
                        dependencies : [pipewire_dep],
                        install : false)
test('pw-test-cpp', test_cpp)
endif
