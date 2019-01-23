audioconvert_sources = ['fmtconvert.c',
			'channelmix.c',
			'resample.c',
			'splitter.c',
			'merger.c',
			'audioconvert.c',
			'plugin.c']

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
                          include_directories : [spa_inc],
                          dependencies : [ speexdsp_dep, mathlib ],
                          install : true,
                          install_dir : '@0@/spa/audioconvert/'.format(get_option('libdir')))

test_apps = [
	'test-fmt-ops',
]

foreach a : test_apps
  test(a,
	executable(a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach

benchmark_apps = [
	'benchmark-fmt-ops',
]

foreach a : benchmark_apps
  benchmark(a,
	executable(a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach
