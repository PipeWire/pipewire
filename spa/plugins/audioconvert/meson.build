audioconvert_sources = ['fmtconvert.c', 'audioconvert.c', 'channelmix.c', 'plugin.c']

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
                          include_directories : [spa_inc, spa_libinc],
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa/audioconvert/'.format(get_option('libdir')))
