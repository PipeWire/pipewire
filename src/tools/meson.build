tools_sources = [
  [ 'pw-mon', [ 'pw-mon.c' ] ],
  [ 'pw-config', [ 'pw-config.c' ] ],
  [ 'pw-dot', [ 'pw-dot.c' ] ],
  [ 'pw-dump', [ 'pw-dump.c' ] ],
  [ 'pw-profiler', [ 'pw-profiler.c' ] ],
  [ 'pw-mididump', [ 'pw-mididump.c', 'midifile.c' ] ],
  [ 'pw-metadata', [ 'pw-metadata.c' ] ],
  [ 'pw-loopback', [ 'pw-loopback.c' ] ],
  [ 'pw-link', [ 'pw-link.c' ] ],
]

foreach t : tools_sources
  executable(t.get(0),
    t.get(1),
    install: true,
    dependencies : [pipewire_dep, mathlib],
  )
endforeach

executable('pw-cli',
  'pw-cli.c',
  install: true,
  dependencies: [pipewire_dep, readline_dep]
)

if ncurses_dep.found()
  executable('pw-top',
    'pw-top.c',
    install: true,
    dependencies : [pipewire_dep, ncurses_dep],
  )
endif

build_pw_cat = false
build_pw_cat_with_ffmpeg = false
pwcat_deps = [ sndfile_dep ]

if get_option('pw-cat').allowed() and sndfile_dep.found()
  build_pw_cat = true

  if pw_cat_ffmpeg.allowed() and avcodec_dep.found() and avformat_dep.found() and avutil_dep.found()
    pwcat_deps += [avcodec_dep, avformat_dep, avutil_dep]
    build_pw_cat_with_ffmpeg = true
  endif

  pwcat_sources = [
    'pw-cat.c',
    'midifile.c',
    'dfffile.c',
    'dsffile.c',
  ]

  pwcat_aliases = [
    'pw-play',
    'pw-record',
    'pw-midiplay',
    'pw-midirecord',
    'pw-dsdplay',
    'pw-encplay',
  ]

  pw_cat = executable('pw-cat',
    pwcat_sources,
    install: true,
    dependencies : [pwcat_deps, pipewire_dep, mathlib],
  )

  foreach alias : pwcat_aliases
    custom_target(
      alias,
      build_by_default: true,
      install: false,
      command: [ln, '-sf', meson.project_build_root() + '/@INPUT@', '@OUTPUT@'],
      input: pw_cat,
      output: alias,
    )
    install_symlink(
      alias,
      pointing_to: pw_cat.name(),
      install_dir: pipewire_bindir,
    )
  endforeach
elif not sndfile_dep.found() and get_option('pw-cat').enabled()
  error('pw-cat is enabled but required dependency `sndfile` was not found.')
endif
summary({'Build pw-cat tool': build_pw_cat}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
if build_pw_cat
  summary({'Build pw-cat with FFmpeg integration': build_pw_cat_with_ffmpeg}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
endif

if dbus_dep.found()
  executable('pw-reserve',
    'reserve.h',
    'reserve.c',
    'pw-reserve.c',
    install: true,
    dependencies : [dbus_dep, pipewire_dep],
  )
endif
