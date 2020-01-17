executable('pipewire-monitor',
	'pipewire-monitor.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)
executable('pipewire-cli',
	'pipewire-cli.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)
executable('pipewire-dot',
	'pipewire-dot.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep],
)

if get_option('pwcat') and sndfile_dep.found()

  pwcat_sources = [
    'pwcat.c',
  ]

  pwcat_aliases = [
    'pwplay',
    'pwrecord',
  ]

  executable('pwcat',
    pwcat_sources,
    c_args : [ '-D_GNU_SOURCE' ],
    install: true,
    dependencies : [sndfile_dep, pipewire_dep, mathlib],
  )

  foreach alias : pwcat_aliases
    dst = join_paths(pipewire_bindir, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pwcat', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach

endif
