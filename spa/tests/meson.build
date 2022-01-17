# Generate a compilation test for each SPA header, excluding the type-info.h
# ones which have circular dependencies and take some effort to fix.
# Do it for C++ if possible (picks up C++-specific errors), otherwise for C.
find = find_program('find', required: false)
summary({'find (for header testing)': find.found()}, bool_yn: true, section: 'Optional programs')
if find.found()
  spa_headers = run_command(find,
                            meson.project_source_root() / 'spa' / 'include',
                            '-name', '*.h',
                            '-not', '-name', 'type-info.h',
                            '-type', 'f',
                            '-printf', '%P\n',
                            check: false)
  foreach spa_header : spa_headers.stdout().split('\n')
    if spa_header.endswith('.h')  # skip empty lines
      ext = have_cpp ? 'cpp' : 'c'
      src = configure_file(input: 'spa-include-test-template.c',
                           output: 'spa-include-test-@0@.@1@'.format(spa_header.underscorify(), ext),
                           configuration: {
                             'INCLUDE': spa_header,
                           })
      executable('spa-include-test-@0@'.format(spa_header.underscorify()),
                 src,
                 dependencies: [ spa_dep ],
                 install: false)
    endif
  endforeach
endif

benchmark_apps = [
  'stress-ringbuffer',
  'benchmark-pod',
  'benchmark-dict',
]

foreach a : benchmark_apps
  benchmark('spa-' + a,
    executable('spa-' + a, a + '.c',
      dependencies : [ spa_dep, dl_lib, pthread_lib, mathlib ],
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir,
    ),
    env : [
      'SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
    ]
  )

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'spa-' + a)
    configure_file(
      input: installed_tests_template,
      output: 'spa-' + a + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf,
    )
  endif
endforeach
