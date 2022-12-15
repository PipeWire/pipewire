audioconvert_sources = [
  'audioadapter.c',
  'audioconvert.c',
  'plugin.c'
]

simd_cargs = []
simd_dependencies = []

audioconvert_c = static_library('audioconvert_c',
  [ 'channelmix-ops-c.c',
    'biquad.c',
    'crossover.c',
    'volume-ops-c.c',
    'peaks-ops-c.c',
    'resample-native-c.c',
    'fmt-ops-c.c' ],
  c_args : ['-Ofast', '-ffast-math'],
  dependencies : [ spa_dep ],
  install : false
  )
simd_dependencies += audioconvert_c

if have_sse
  audioconvert_sse = static_library('audioconvert_sse',
    ['resample-native-sse.c',
      'volume-ops-sse.c',
      'peaks-ops-sse.c',
      'channelmix-ops-sse.c' ],
    c_args : [sse_args, '-Ofast', '-DHAVE_SSE'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_SSE']
  simd_dependencies += audioconvert_sse
endif
if have_sse2
  audioconvert_sse2 = static_library('audioconvert_sse2',
    ['fmt-ops-sse2.c' ],
    c_args : [sse2_args, '-O3', '-DHAVE_SSE2'],
    dependencies : [ spa_dep ],
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
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_SSSE3']
  simd_dependencies += audioconvert_ssse3
endif
if have_sse41
  audioconvert_sse41 = static_library('audioconvert_sse41',
    ['fmt-ops-sse41.c'],
    c_args : [sse41_args, '-O3', '-DHAVE_SSE41'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_SSE41']
  simd_dependencies += audioconvert_sse41
endif
if have_avx and have_fma
  audioconvert_avx = static_library('audioconvert_avx',
    ['resample-native-avx.c'],
    c_args : [avx_args, fma_args, '-O3', '-DHAVE_AVX', '-DHAVE_FMA'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_AVX', '-DHAVE_FMA']
  simd_dependencies += audioconvert_avx
endif
if have_avx2
  audioconvert_avx2 = static_library('audioconvert_avx2',
    ['fmt-ops-avx2.c'],
    c_args : [avx2_args, '-O3', '-DHAVE_AVX2'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_AVX2']
  simd_dependencies += audioconvert_avx2
endif

if have_neon
  audioconvert_neon = static_library('audioconvert_neon',
    ['resample-native-neon.c',
      'fmt-ops-neon.c' ],
    c_args : [neon_args, '-O3', '-DHAVE_NEON'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_NEON']
  simd_dependencies += audioconvert_neon
endif

audioconvert_lib = static_library('audioconvert',
  ['fmt-ops.c',
    'channelmix-ops.c',
    'peaks-ops.c',
    'resample-native.c',
    'resample-peaks.c',
    'wavfile.c',
    'volume-ops.c' ],
  c_args : [ simd_cargs, '-O3'],
  link_with : simd_dependencies,
  include_directories : [configinc],
  dependencies : [ spa_dep ],
  install : false
  )
audioconvert_dep = declare_dependency(link_with: audioconvert_lib)

spa_audioconvert_lib = shared_library('spa-audioconvert',
  audioconvert_sources,
  c_args : simd_cargs,
  dependencies : [ spa_dep, mathlib, audioconvert_dep ],
  install : true,
  install_dir : spa_plugindir / 'audioconvert')
spa_audioconvert_dep = declare_dependency(link_with: spa_audioconvert_lib)

test_lib = static_library('test_lib',
  ['test-source.c' ],
  c_args : ['-O3'],
  dependencies : [ spa_dep ],
  install : false
  )

test_apps = [
  'test-audioadapter',
  'test-audioconvert',
  'test-channelmix',
  'test-fmt-ops',
  'test-peaks',
  'test-resample',
  ]

foreach a : test_apps
  test(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib, audioconvert_dep, spa_audioconvert_dep ],
      include_directories : [ configinc ],
      link_with : [ test_lib ],
      install_rpath : spa_plugindir / 'audioconvert',
      c_args : [ simd_cargs ],
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'audioconvert'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'audioconvert' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'audioconvert',
        configuration: test_conf
        )
  endif
endforeach

benchmark_apps = [
  'benchmark-fmt-ops',
  'benchmark-resample',
  ]

foreach a : benchmark_apps
  benchmark(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib, audioconvert_dep, spa_audioconvert_dep ],
      include_directories : [ configinc ],
      c_args : [ simd_cargs ],
      install_rpath : spa_plugindir / 'audioconvert',
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'audioconvert'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'audioconvert' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'audioconvert',
        configuration: test_conf
        )
  endif
endforeach

if sndfile_dep.found()
  sparesample_sources = [
    'spa-resample.c',
    ]
  executable('spa-resample',
    sparesample_sources,
    link_with : [ test_lib ],
    dependencies : [ spa_dep, sndfile_dep, mathlib, audioconvert_dep ],
    install : true,
    )
endif
