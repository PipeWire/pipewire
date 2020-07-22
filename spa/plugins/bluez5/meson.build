
bluez5_sources = ['plugin.c',
		  'a2dp-codecs.c',
		  'a2dp-sink.c',
		  'a2dp-source.c',
		  'sco-sink.c',
		  'sco-source.c',
		  'bluez5-device.c',
		  'bluez5-dbus.c']

if get_option('bluez5-backend-native')
  bluez5_sources += ['backend-hsp-native.c']
endif

if get_option('bluez5-backend-ofono')
  bluez5_sources += ['backend-ofono.c']
endif

if get_option('bluez5-backend-hsphfpd')
  bluez5_sources += ['backend-hsphfpd.c']
endif

bluez5lib = shared_library('spa-bluez5',
	bluez5_sources,
	include_directories : [ spa_inc, configinc ],
	c_args : [ '-D_GNU_SOURCE' ],
	dependencies : [ dbus_dep, sbc_dep, bluez_dep ],
	install : true,
        install_dir : join_paths(spa_plugindir, 'bluez5'))
