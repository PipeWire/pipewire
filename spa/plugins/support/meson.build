spa_support_sources = ['mapper.c',
		       'logger.c',
		       'loop.c',
		       'plugin.c']

spa_support_lib = shared_library('spa-support',
                          spa_support_sources,
                          include_directories : [ spa_inc, spa_libinc],
                          dependencies : threads_dep,
                          install : true,
                          install_dir : '@0@/spa/support'.format(get_option('libdir')))
