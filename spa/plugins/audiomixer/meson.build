audiomixer_sources = [
  'audiomixer.c',
  'mixer-dsp.c',
  'plugin.c'
]

simd_cargs = []
simd_dependencies = []

audiomixer_c = static_library('audiomixer_c',
  ['mix-ops-c.c' ],
  c_args : ['-O3'],
  dependencies : [ spa_dep ],
  install : false
)
simd_dependencies += audiomixer_c

if have_sse
  audiomixer_sse = static_library('audiomixer_sse',
    ['mix-ops-sse.c' ],
    c_args : [sse_args, '-O3', '-DHAVE_SSE'],
    dependencies : [ spa_dep ],
    install : false
  )
  simd_cargs += ['-DHAVE_SSE']
  simd_dependencies += audiomixer_sse
endif
if have_sse2
  audiomixer_sse2 = static_library('audiomixer_sse2',
    ['mix-ops-sse2.c' ],
    c_args : [sse2_args, '-O3', '-DHAVE_SSE2'],
    dependencies : [ spa_dep ],
    install : false
  )
  simd_cargs += ['-DHAVE_SSE2']
  simd_dependencies += audiomixer_sse2
endif
if have_avx and have_fma
  audiomixer_avx = static_library('audiomixer_avx',
    ['mix-ops-avx.c'],
    c_args : [avx_args, fma_args, '-O3', '-DHAVE_AVX', '-DHAVE_FMA'],
    dependencies : [ spa_dep ],
    install : false
  )
  simd_cargs += ['-DHAVE_AVX', '-DHAVE_FMA']
  simd_dependencies += audiomixer_avx
endif

audiomixer_lib = static_library('audiomixer',
  ['mix-ops.c' ],
  c_args : [ simd_cargs, '-O3'],
  link_with : simd_dependencies,
  include_directories : [configinc],
  dependencies : [ spa_dep ],
  install : false
  )
audiomixer_dep = declare_dependency(link_with: audiomixer_lib)

spa_audiomixer_lib = shared_library('spa-audiomixer',
  audiomixer_sources,
  c_args : simd_cargs,
  link_with : simd_dependencies,
  dependencies : [ spa_dep, mathlib, audiomixer_dep ],
  install : true,
  install_dir : spa_plugindir / 'audiomixer'
)
spa_audiomixer_dep = declare_dependency(link_with: spa_audiomixer_lib)

test_apps = [
  'test-mix-ops',
  ]

foreach a : test_apps
  test(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib, audiomixer_dep ],
      include_directories : [ configinc, test_inc ],
      link_with : [ test_lib ],
      install_rpath : spa_plugindir / 'audiomixer',
      c_args : [ simd_cargs ],
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'audiomixer'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'audiomixer' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'audiomixer',
        configuration: test_conf
        )
  endif
endforeach

benchmark_apps = [
  'benchmark-mix-ops',
  ]

foreach a : benchmark_apps
  benchmark(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib, audiomixer_dep ],
      include_directories : [ configinc, test_inc ],
      c_args : [ simd_cargs ],
      install_rpath : spa_plugindir / 'audiomixer',
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'audiomixer'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'audiomixer' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'audiomixer',
        configuration: test_conf
        )
  endif
endforeach
