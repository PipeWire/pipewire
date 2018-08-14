videotestsrc_sources = ['videotestsrc.c', 'plugin.c']

videotestsrclib = shared_library('spa-videotestsrc',
                                 videotestsrc_sources,
                                 include_directories : [ spa_inc],
                                 dependencies : threads_dep,
                                 install : true,
                                 install_dir : '@0@/spa/videotestsrc'.format(get_option('libdir')))
