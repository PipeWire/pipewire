# Examples, in order from simple to complicated
spa_examples = [
  'adapter-control',
  'example-control',
  'local-libcamera',
  'local-v4l2',
]

spa_examples_extra_deps = {
  'local-v4l2': [sdl_dep],
  'local-libcamera': [sdl_dep, libcamera_dep],
}

foreach c : spa_examples
  deps = spa_examples_extra_deps.get(c, [])

  found = true
  foreach dep : deps
    found = found and dep.found()
  endforeach

  if found
    executable(
      c,
      c + '.c',
      include_directories : [configinc],
      dependencies : [spa_dep, dl_lib, pthread_lib, mathlib] + deps,
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'examples' / 'spa'
    )
  endif
endforeach
