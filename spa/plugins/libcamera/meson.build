libcamera_sources = ['libcamera.c',
                'libcamera-device.c',
                'libcamera-client.c',
                'libcamera-source.c',
                'libcamera_wrapper.cpp']

libcameralib = shared_library('spa-libcamera',
                          libcamera_sources,
                          include_directories : [ spa_inc ],
                          dependencies : [ libudev_dep, libcamera_dep, pthread_lib  ],
                          install : true,
		          install_dir : join_paths(spa_plugindir, 'libcamera'))
