spa_logger_sources = ['logger.c', 'plugin.c']

spa_logger_lib = shared_library('spa-logger',
                          spa_logger_sources,
                          include_directories : [ spa_inc, spa_libinc],
                          dependencies : threads_dep,
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa'.format(get_option('libdir')))
