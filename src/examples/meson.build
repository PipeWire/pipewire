executable('video-src',
  'video-src.c',
  install: false,
  dependencies : [pipewire_dep],
)
executable('audio-src',
  'audio-src.c',
  install: false,
  dependencies : [pipewire_dep, mathlib],
)
executable('export-source',
  'export-source.c',
  install: false,
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa',
  'export-spa.c',
  install: false,
  dependencies : [pipewire_dep, mathlib],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    install: false,
    dependencies : [pipewire_dep, sdl_dep],
  )
  executable('local-v4l2',
    'local-v4l2.c',
    install: false,
    dependencies : [pipewire_dep, sdl_dep],
  )
  executable('export-sink',
    'export-sink.c',
    install: false,
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
endif
