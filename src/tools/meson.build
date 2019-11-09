executable('pipewire-monitor',
	'pipewire-monitor.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep, epoll_shim_dep],
)
executable('pipewire-cli',
	'pipewire-cli.c',
	c_args : [ '-D_GNU_SOURCE' ],
	install: true,
	dependencies : [pipewire_dep, epoll_shim_dep],
)
