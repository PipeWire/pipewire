videotestsrc_sources = ['videotestsrc.c', 'plugin.c']

videotestsrclib = shared_library('spa-videotestsrc',
                                 videotestsrc_sources,
                                 include_directories : [ spa_inc, spa_libinc],
                                 dependencies : threads_dep,
                                 link_with : spalib,
                                 install : true,
                                 install_dir : '@0@/spa'.format(get_option('libdir')))
