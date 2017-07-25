executable('video-src',
  'video-src.c',
  install: false,
  dependencies : [pipewire_dep],
)
executable('export-v4l2',
  'export-v4l2.c',
  install: false,
  dependencies : [pipewire_dep],
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
    dependencies : [pipewire_dep, sdl_dep],
  )
endif
