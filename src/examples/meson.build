executable('video-src',
  'video-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)
executable('video-src-alloc',
  'video-src-alloc.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)
executable('audio-src',
  'audio-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)
executable('export-source',
  'export-source.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)

executable('audio-dsp-filter',
  'audio-dsp-filter.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)

executable('export-spa',
  'export-spa.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)

executable('export-spa-device',
  'export-spa-device.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)

executable('media-session',
  'media-session/media-session.c',
  'media-session/monitor.c',
  'media-session/policy.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [dbus_dep, pipewire_dep, alsa_dep, mathlib, epoll_shim_dep],
)

executable('bluez-session',
  'bluez-session.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install: false,
  dependencies : [pipewire_dep, mathlib, epoll_shim_dep],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-dsp-play',
    'video-dsp-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [pipewire_dep, sdl_dep, mathlib],
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
