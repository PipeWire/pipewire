audiotestsrc_sources = ['audiotestsrc.c', 'plugin.c']

audiotestsrclib = shared_library('spa-audiotestsrc',
                          audiotestsrc_sources,
                          include_directories : [spa_inc],
                          dependencies : mathlib,
                          install : true,
                          install_dir : '@0@/spa/audiotestsrc'.format(get_option('libdir')))
