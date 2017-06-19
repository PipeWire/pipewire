test_sources = ['fakesrc.c', 'fakesink.c', 'plugin.c']

testlib = shared_library('spa-test',
                          test_sources,
                          include_directories : [ spa_inc, spa_libinc],
                          dependencies : threads_dep,
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa/test'.format(get_option('libdir')))
