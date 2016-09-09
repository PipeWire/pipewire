videotestsrc_sources = ['videotestsrc.c', 'plugin.c']

videotestsrclib = shared_library('spa-videotestsrc',
                                 videotestsrc_sources,
                                 include_directories : inc,
                                 dependencies : threads_dep,
                                 link_with : spalib,
                                 install : true)
