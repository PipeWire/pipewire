spa_support_sources = ['cpu.c',
		       'logger.c',
		       'loop.c',
		       'plugin.c',
		       'system.c']

spa_support_lib = shared_library('spa-support',
			spa_support_sources,
			c_args : [ '-D_GNU_SOURCE' ],
			include_directories : [ spa_inc ],
			dependencies : [ pthread_lib, epoll_shim_dep ],
			install : true,
		        install_dir : join_paths(spa_plugindir, 'support'))


if get_option('evl')
  evl_inc = include_directories('/usr/evl/include')
  evl_lib = cc.find_library('evl',
		dirs: ['/usr/evl/lib/'])

  spa_evl_sources = ['evl-system.c',
		   'evl-plugin.c']

  spa_evl_lib = shared_library('spa-evl',
			spa_evl_sources,
			c_args : [ '-D_GNU_SOURCE' ],
			include_directories : [ spa_inc, evl_inc],
			dependencies : [ pthread_lib, evl_lib],
			install : true,
		        install_dir : join_paths(spa_plugindir, 'support'))
endif

spa_dbus_sources = ['dbus.c']

spa_dbus_lib = shared_library('spa-dbus',
			spa_dbus_sources,
			include_directories : [ spa_inc],
			dependencies : [dbus_dep, ],
			install : true,
		        install_dir : join_paths(spa_plugindir, 'support'))
