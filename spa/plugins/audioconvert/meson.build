audioconvert_sources = ['audioadapter.c',
			'audioconvert.c',
			'fmtconvert.c',
			'fmt-ops.c',
			'channelmix.c',
			'channelmix-ops.c',
			'merger.c',
			'plugin.c',
			'resample.c',
			'splitter.c']

simd_cargs = []
simd_dependencies = []

audioconvert_c = static_library('audioconvert_c',
	['resample-native-c.c',
	 'channelmix-ops-c.c',
	 'fmt-ops-c.c' ],
	c_args : ['-O3'],
	include_directories : [spa_inc],
	install : false
)
simd_dependencies += audioconvert_c

if have_sse
	audioconvert_sse = static_library('audioconvert_sse',
		['resample-native-sse.c',
		 'channelmix-ops-sse.c' ],
		c_args : [sse_args, '-O3', '-DHAVE_SSE'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSE']
	simd_dependencies += audioconvert_sse
endif
if have_sse2
	audioconvert_sse2 = static_library('audioconvert_sse2',
		['fmt-ops-sse2.c' ],
		c_args : [sse2_args, '-O3', '-DHAVE_SSE2'],
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
		c_args : [ssse3_args, '-O3', '-DHAVE_SSSE3'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSSE3']
	simd_dependencies += audioconvert_ssse3
endif
if have_sse41
	audioconvert_sse41 = static_library('audioconvert_sse41',
		['fmt-ops-sse41.c'],
		c_args : [sse41_args, '-O3', '-DHAVE_SSE41'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_SSE41']
	simd_dependencies += audioconvert_sse41
endif
if have_avx and have_fma
	audioconvert_avx = static_library('audioconvert_avx',
		['resample-native-avx.c'],
		c_args : [avx_args, fma_args, '-O3', '-DHAVE_AVX', '-DHAVE_FMA'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_AVX', '-DHAVE_FMA']
	simd_dependencies += audioconvert_avx
endif
if have_avx2
	audioconvert_avx2 = static_library('audioconvert_avx2',
		['fmt-ops-avx2.c'],
		c_args : [avx2_args, '-O3', '-DHAVE_AVX2'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_AVX2']
	simd_dependencies += audioconvert_avx2
endif

if have_neon
	audioconvert_neon = static_library('audioconvert_neon',
		['resample-native-neon.c',
		 'fmt-ops-neon.c' ],
		c_args : ['-O3', '-DHAVE_NEON'],
		include_directories : [spa_inc],
		install : false
	)
	simd_cargs += ['-DHAVE_NEON']
	simd_dependencies += audioconvert_neon
endif

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
			  c_args : simd_cargs,
                          include_directories : [spa_inc],
                          dependencies : [ mathlib ],
			  link_with : simd_dependencies,
                          install : true,
                          install_dir : join_paths(spa_plugindir, 'audioconvert'))

test_lib = static_library('test_lib',
	['test-source.c' ],
	c_args : ['-O3'],
	include_directories : [spa_inc],
	install : false
)

test_apps = [
	'test-audioadapter',
	'test-audioconvert',
	'test-channelmix',
	'test-fmt-ops',
	'test-resample',
]

foreach a : test_apps
  test(a,
	executable(a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		link_with : [ simd_dependencies, test_lib, audioconvertlib ],
		c_args : [ simd_cargs, '-D_GNU_SOURCE' ],
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
		dependencies : [dl_lib, pthread_lib, mathlib, ],
		include_directories : [spa_inc ],
		c_args : [ simd_cargs, '-D_GNU_SOURCE' ],
		link_with : simd_dependencies,
		install : false),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])
endforeach

if sndfile_dep.found()
  sparesample_sources = [
    'spa-resample.c',
  ]
  executable('spa-resample',
    sparesample_sources,
    c_args : [ simd_cargs, '-D_GNU_SOURCE' ],
    install: false,
    include_directories : [spa_inc ],
    link_with : [ simd_dependencies, test_lib, audioconvertlib ],
    dependencies : [sndfile_dep, mathlib],
  )
endif
