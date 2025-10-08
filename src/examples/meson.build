# Examples, in order from simple to complicated
examples = [
  'audio-src',
  'audio-src-ring',
  'audio-src-ring2',
  'audio-dsp-src',
  'audio-dsp-filter',
  'audio-dsp-sink',
  'audio-dsp-sink2',
  'audio-capture',
  'video-play',
  'video-src',
  'video-src-sync',
  'video-dsp-play',
  'video-dsp-src',
  'video-play-pull',
  'video-play-reneg',
  'video-play-sync',
  'video-src-alloc',
  'video-src-reneg',
  'video-src-fixate',
  'video-play-fixate',
  'midi-src',
  'internal',
  'export-sink',
  'export-source',
  'export-spa',
  'export-spa-device',
  'bluez-session',
  'local-v4l2',
  'gmain',
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
  'video-play-sync': [sdl_dep],
  'video-dsp-play': [sdl_dep],
  'local-v4l2': [sdl_dep],
  'export-sink': [sdl_dep],
  'gmain': [glib2_dep],
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
