tools_sources = [
  [ 'pw-mon', [ 'pw-mon.c' ] ],
  [ 'pw-cli', [ 'pw-cli.c' ] ],
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

if ncurses_dep.found()
  executable('pw-top',
	'pw-top.c',
	install: true,
	dependencies : [pipewire_dep, ncurses_dep],
  )
endif

build_pw_cat = false
if not get_option('pw-cat').disabled() and sndfile_dep.found()
  build_pw_cat = true

  pwcat_sources = [
    'pw-cat.c',
    'midifile.c',
  ]

  pwcat_aliases = [
    'pw-play',
    'pw-record',
    'pw-midiplay',
    'pw-midirecord',
  ]

  executable('pw-cat',
    pwcat_sources,
    install: true,
    dependencies : [sndfile_dep, pipewire_dep, mathlib],
  )

  foreach alias : pwcat_aliases
    dst = pipewire_bindir / alias
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pw-cat', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
elif not sndfile_dep.found() and get_option('pw-cat').enabled()
	error('pw-cat is enabled but required dependency `sndfile` was not found.')
endif
summary({'Build pw-cat tool': build_pw_cat}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
