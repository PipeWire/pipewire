
bluez5_sources = ['plugin.c',
		  'a2dp-sink.c',
                  'bluez5-monitor.c']

bluez5lib = shared_library('spa-bluez5',
	bluez5_sources,
	include_directories : [ spa_inc, spa_libinc ],
	dependencies : [ dbus_dep, sbc_dep ],
	install : true,
	install_dir : '@0@/spa/bluez5'.format(get_option('libdir')))
