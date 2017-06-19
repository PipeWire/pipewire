audiomixer_sources = ['audiomixer.c', 'plugin.c']

audiomixerlib = shared_library('spa-audiomixer',
                          audiomixer_sources,
                          include_directories : [spa_inc, spa_libinc],
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa/audiomixer/'.format(get_option('libdir')))
