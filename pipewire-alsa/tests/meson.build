test_apps = [
  [ 'test-pipewire-alsa-stress', [alsa_dep, pthread_lib] ],
]

foreach a : test_apps
  executable('pw-' + a[0], a[0] + '.c',
    dependencies : a[1],
    include_directories: [includes_inc],
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir
  )

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'pw-' + a[0])
    configure_file(
      input: installed_tests_template,
      output: 'pw-' + a[0] + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif
endforeach
