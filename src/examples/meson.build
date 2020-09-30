executable('video-src',
  'video-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)
executable('video-src-alloc',
  'video-src-alloc.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)
executable('video-src-reneg',
  'video-src-reneg.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)
executable('audio-src',
  'audio-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)
executable('audio-dsp-src',
  'audio-dsp-src.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)
executable('export-source',
  'export-source.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)

executable('audio-dsp-filter',
  'audio-dsp-filter.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa',
  'export-spa.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)

executable('export-spa-device',
  'export-spa-device.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)

if alsa_dep.found()
  executable('pipewire-media-session',
    'media-session/access-flatpak.c',
    'media-session/access-portal.c',
    'media-session/alsa-midi.c',
    'media-session/alsa-monitor.c',
    'media-session/alsa-endpoint.c',
    'media-session/bluez-monitor.c',
    'media-session/bluez-endpoint.c',
    'media-session/default-nodes.c',
    'media-session/default-profile.c',
    'media-session/default-routes.c',
    'media-session/media-session.c',
    'media-session/session-manager.c',
    'media-session/metadata.c',
    'media-session/pulse-bridge.c',
    'media-session/stream-endpoint.c',
    'media-session/restore-stream.c',
    'media-session/policy-ep.c',
    'media-session/policy-node.c',
    'media-session/v4l2-monitor.c',
    'media-session/v4l2-endpoint.c',
    'media-session/libcamera-monitor.c',
    'media-session/suspend-node.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: true,
    dependencies : [dbus_dep, pipewire_dep, alsa_dep, mathlib],
)
endif

executable('pw-reserve',
	'media-session/pw-reserve.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [dbus_dep, pipewire_dep],
)

executable('bluez-session',
  'bluez-session.c',
  c_args : [ '-D_GNU_SOURCE' ],
  install : installed_tests_enabled,
  install_dir : join_paths(installed_tests_execdir, 'examples'),
  dependencies : [pipewire_dep, mathlib],
)

if sdl_dep.found()
  executable('video-play',
    'video-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install : installed_tests_enabled,
    install_dir : join_paths(installed_tests_execdir, 'examples'),
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-play-reneg',
    'video-play-reneg.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install : installed_tests_enabled,
    install_dir : join_paths(installed_tests_execdir, 'examples'),
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
  executable('video-dsp-play',
    'video-dsp-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install : installed_tests_enabled,
    install_dir : join_paths(installed_tests_execdir, 'examples'),
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )

  executable('local-v4l2',
    'local-v4l2.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install : installed_tests_enabled,
    install_dir : join_paths(installed_tests_execdir, 'examples'),
    dependencies : [pipewire_dep, sdl_dep],
  )

  executable('export-sink',
    'export-sink.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install : installed_tests_enabled,
    install_dir : join_paths(installed_tests_execdir, 'examples'),
    dependencies : [pipewire_dep, sdl_dep, mathlib],
  )
endif
