remote_sources = ['proxy.c',
                  'dbus-proxy.c',
                  'plugin.c']

remotelib = shared_library('spa-remote',
                           remote_sources,
                           include_directories : spa_inc,
                           link_with : spalib,
                           install : true,
                           install_dir : '@0@/spa'.format(get_option('libdir')))
