executable('video-src',
  'video-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib],
)
executable('audio-src',
  'audio-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib],
)
executable('export-source',
  'export-source.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa',
  'export-spa.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib],
)

executable('media-session',
  'media-session.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [pipewire_dep, sdl_dep],
  )
  executable('local-v4l2',
    'local-v4l2.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [pipewire_dep, sdl_dep],
  )
  executable('export-sink',
    'export-sink.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
endif
