executable('video-src',
  'video-src.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)
executable('video-src-alloc',
  'video-src-alloc.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)
executable('video-src-reneg',
  'video-src-reneg.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)
executable('audio-src',
  'audio-src.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)
executable('audio-dsp-src',
  'audio-dsp-src.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)
executable('export-source',
  'export-source.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)

executable('audio-dsp-filter',
  'audio-dsp-filter.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa',
  'export-spa.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa-device',
  'export-spa-device.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)

executable('bluez-session',
  'bluez-session.c',
  install : installed_tests_enabled,
  install_dir : installed_tests_execdir / 'examples',
  dependencies : [pipewire_dep, mathlib],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-play-reneg',
    'video-play-reneg.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-play-pull',
    'video-play-pull.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-dsp-play',
    'video-dsp-play.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )

  executable('local-v4l2',
    'local-v4l2.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep],
  )

  executable('export-sink',
    'export-sink.c',
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples',
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
endif
