spa_support_sources = ['cpu.c',
		       'logger.c',
		       'loop.c',
		       'plugin.c']

spa_support_lib = shared_library('spa-support',
			spa_support_sources,
			c_args : [ '-D_GNU_SOURCE' ],
			include_directories : [ spa_inc],
			dependencies : threads_dep,
			install : true,
			install_dir : '@0@/spa/support'.format(get_option('libdir')))

spa_dbus_sources = ['dbus.c']

spa_dbus_lib = shared_library('spa-dbus',
			spa_dbus_sources,
			include_directories : [ spa_inc],
			dependencies : dbus_dep,
			install : true,
			install_dir : '@0@/spa/support'.format(get_option('libdir')))
