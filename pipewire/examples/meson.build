executable('video-src',
  'video-src.c',
  install: false,
  dependencies : [pipewire_dep],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    install: false,
    dependencies : [pipewire_dep,sdl_dep],
  )
endif
