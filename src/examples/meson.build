# Examples, in order from simple to complicated
examples = [
  'audio-src',
  'audio-dsp-src',
  'audio-dsp-filter',
  'audio-capture',
  'video-play',
  'video-src',
  'video-dsp-play',
  'video-play-pull',
  'video-play-reneg',
  'video-src-alloc',
  'video-src-reneg',
  'video-src-fixate',
  'video-play-fixate',
  'internal',
  'export-sink',
  'export-source',
  'export-spa',
  'export-spa-device',
  'bluez-session',
  'local-v4l2',
]

if not get_option('examples').allowed()
  subdir_done()
endif

examples_extra_deps = {
  'video-src-fixate': [drm_dep],
  'video-play': [sdl_dep],
  'video-play-reneg': [sdl_dep],
  'video-play-fixate': [sdl_dep, drm_dep],
  'video-play-pull': [sdl_dep],
  'video-dsp-play': [sdl_dep],
  'local-v4l2': [sdl_dep],
  'export-sink': [sdl_dep],
}

foreach c : examples
  deps = examples_extra_deps.get(c, [])

  found = true
  foreach dep : deps
    found = found and dep.found()
  endforeach

  if found
    executable(
      c, c + '.c',
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir / 'examples',
      dependencies : [pipewire_dep, mathlib] + deps,
    )
  endif
endforeach
