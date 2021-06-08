test_apps = [
	'test-buffer',
	'test-node',
	'test-pod',
	'test-utils',
	'test-json',
]

foreach a : test_apps
  test('spa-' + a,
	executable('spa-' + a, a + '.c',
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc, includes_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : installed_tests_enabled,
		install_dir : installed_tests_execdir),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'spa-' + a)
    configure_file(
      input: installed_tests_template,
      output: 'spa-' + a + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif
endforeach

if have_cpp
  gen_cpp_test = find_program('gen-cpp-test.py')
  src_test_cpp = custom_target('gen-cpp-test',
                               command: [gen_cpp_test, meson.source_root() / 'spa' / 'include' / 'spa'],
                               output: 'test-cpp.cpp',
                               capture: true)
  # compilation test only
  executable('spa-test-cpp', src_test_cpp,
             include_directories : [spa_inc ],
             dependencies : [],
             install : false)
endif

# Generate a compilation test for each SPA header, excluding the type-info.h
# ones which have circular dependencies and take some effort to fix.
find = find_program('find', required: false)
if find.found()
  spa_headers = run_command(find_program('find'),
                            meson.source_root() / 'spa' / 'include',
                            '-name', '*.h',
                            '-not', '-name', 'type-info.h',
                            '-type', 'f',
                            '-printf', '%P\n')
  foreach spa_header : spa_headers.stdout().split('\n')
    if spa_header.endswith('.h')  # skip empty lines
      c = configuration_data()
      c.set('INCLUDE', spa_header)
      src = configure_file(input: 'spa-include-test-template.c',
                          output: 'spa-include-test-@0@.c'.format(spa_header.underscorify()),
                          configuration: c)
      executable('spa-include-test-@0@'.format(spa_header.underscorify()),
                 src,
                 include_directories: [spa_inc],
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
		dependencies : [dl_lib, pthread_lib, mathlib ],
		include_directories : [spa_inc ],
		c_args : [ '-D_GNU_SOURCE' ],
		install : installed_tests_enabled,
		install_dir : installed_tests_execdir),
	env : [
		'SPA_PLUGIN_DIR=@0@/spa/plugins/'.format(meson.build_root()),
	])

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'spa-' + a)
    configure_file(
      input: installed_tests_template,
      output: 'spa-' + a + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif
endforeach
