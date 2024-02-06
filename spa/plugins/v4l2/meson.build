v4l2_sources = ['v4l2.c',
                'v4l2-device.c',
                'v4l2-source.c']
v4l2_dependencies = [ spa_dep, libinotify_dep ]

if libudev_dep.found()
v4l2_sources += [ 'v4l2-udev.c' ]
v4l2_dependencies += [ libudev_dep ]
endif

v4l2lib = shared_library('spa-v4l2',
                          v4l2_sources,
                          include_directories : [ configinc ],
                          dependencies : v4l2_dependencies,
                          install : true,
                          install_dir : spa_plugindir / 'v4l2')
