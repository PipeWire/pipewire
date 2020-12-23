
bluez5_sources = ['plugin.c',
		  'a2dp-codecs.c',
		  'a2dp-codec-sbc.c',
		  'a2dp-sink.c',
		  'a2dp-source.c',
		  'sco-sink.c',
		  'sco-source.c',
		  'sco-io.c',
		  'bluez5-device.c',
		  'bluez5-dbus.c']

bluez5_args = [ '-D_GNU_SOURCE' ]
bluez5_deps = [ dbus_dep, sbc_dep, bluez_dep ]

if ldac_dep.found()
  bluez5_sources += [ 'a2dp-codec-ldac.c' ]
  bluez5_args += [ '-DENABLE_LDAC' ]
  bluez5_deps += ldac_dep
  if ldac_abr_dep.found()
    bluez5_args += [ '-DENABLE_LDAC_ABR' ]
    bluez5_deps += ldac_abr_dep
  endif
endif
if aptx_dep.found()
  bluez5_sources += [ 'a2dp-codec-aptx.c' ]
  bluez5_args += [ '-DENABLE_APTX' ]
  bluez5_deps += aptx_dep
endif
if fdk_aac_dep.found()
  bluez5_sources += [ 'a2dp-codec-aac.c' ]
  bluez5_args += [ '-DENABLE_AAC' ]
  bluez5_deps += fdk_aac_dep
endif

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
	c_args : bluez5_args,
	dependencies : bluez5_deps,
	install : true,
        install_dir : join_paths(spa_plugindir, 'bluez5'))
