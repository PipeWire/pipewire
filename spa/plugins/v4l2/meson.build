v4l2_sources = ['v4l2.c',
                'v4l2-monitor.c',
                'v4l2-source.c']

v4l2lib = shared_library('spa-v4l2',
                          v4l2_sources,
                          include_directories : [ spa_inc, spa_libinc ],
                          dependencies : [ v4l2_dep, libudev_dep ],
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa'.format(get_option('libdir')))
