executable('pw-mon',
	'pw-mon.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)
executable('pw-cli',
	'pw-cli.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)
executable('pw-dot',
	'pw-dot.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

executable('pw-dump',
	'pw-dump.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

executable('pw-profiler',
	'pw-profiler.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

executable('pw-mididump',
	[ 'pw-mididump.c', 'midifile.c'],
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep, mathlib],
)

executable('pw-metadata',
	[ 'pw-metadata.c'],
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

executable('pw-loopback',
	[ 'pw-loopback.c'],
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

if ncurses_dep.found()
  executable('pw-top',
	'pw-top.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep, ncurses_dep],
  )
endif

if not get_option('pw-cat').disabled() and sndfile_dep.found()

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
    c_args : [ '-D_GNU_SOURCE' ],
    install: true,
    dependencies : [sndfile_dep, pipewire_dep, mathlib],
  )

  foreach alias : pwcat_aliases
    dst = join_paths(pipewire_bindir, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pw-cat', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
elif not sndfile_dep.found() and get_option('pw-cat').enabled()
	error('pw-cat is enabled but required dependency `sndfile` was not found.')
endif
