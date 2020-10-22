executable('test-volume',
  'test-volume.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib, pulseaudio_dep],
)
