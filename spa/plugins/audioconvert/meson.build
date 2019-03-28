audioconvert_sources = ['fmtconvert.c',
			'channelmix.c',
			'resample.c',
			'splitter.c',
			'merger.c',
			'audioconvert.c',
			'plugin.c']

simd_cargs = []
simd_dependencies = []

if have_sse
	audioconvert_sse = static_library('audioconvert_sse',
		['resample-native-sse.c'],
		c_args : [sse_args],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSE']
	simd_dependencies += audioconvert_sse
endif
if have_sse2
	audioconvert_sse2 = static_library('audioconvert_sse2',
		['fmt-ops-sse2.c'],
		c_args : [sse2_args],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSE2']
	simd_dependencies += audioconvert_sse2
endif
if have_ssse3
	audioconvert_ssse3 = static_library('audioconvert_ssse3',
		['fmt-ops-ssse3.c',
		 'resample-native-ssse3.c' ],
		c_args : [ssse3_args],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSSE3']
	simd_dependencies += audioconvert_ssse3
endif
if have_sse41
	audioconvert_sse41 = static_library('audioconvert_sse41',
		['fmt-ops-sse41.c'],
		c_args : [sse41_args],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSE41']
	simd_dependencies += audioconvert_sse41
endif
if have_avx and have_fma
	audioconvert_avx = static_library('audioconvert_avx',
		['resample-native-avx.c'],
		c_args : [avx2_args, fma_args],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_AVX', '-DHAVE_FMA']
	simd_dependencies += audioconvert_avx
endif

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
			  c_args : simd_cargs,
                          include_directories : [spa_inc],
                          dependencies : [ speexdsp_dep, mathlib ],
			  link_with : simd_dependencies,
                          install : true,
                          install_dir : '@0@/spa/audioconvert/'.format(get_option('libdir')))

test_apps = [
	'test-fmt-ops',
	'test-resample',
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
	'benchmark-resample',
]

foreach a : benchmark_apps
  benchmark(a,
	executable(a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib, speexdsp_dep, ],
		include_directories : [spa_inc ],
		c_args : [ simd_cargs, '-D_GNU_SOURCE' ],
		link_with : simd_dependencies,
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach
