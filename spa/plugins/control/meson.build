control_sources = [
  'mixer.c',
  'plugin.c'
]

controllib = shared_library('spa-control',
  control_sources,
  dependencies : [ spa_dep, mathlib ],
  install : true,
  install_dir : spa_plugindir / 'control')

test_inc = include_directories('../test')

test_apps = [
  'test-mixer-ump-sort',
]

foreach a : test_apps
  test(a,
    executable(a, a + '.c',
      dependencies : [ spa_dep ],
      include_directories : [ configinc, test_inc ],
      install_rpath : spa_plugindir / 'control',
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'control'),
      env : [
        'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
        ])

    if installed_tests_enabled
      test_conf = configuration_data()
      test_conf.set('exec', installed_tests_execdir / 'control' / a)
      configure_file(
        input: installed_tests_template,
        output: a + '.test',
        install_dir: installed_tests_metadir / 'control',
        configuration: test_conf
        )
  endif
endforeach
