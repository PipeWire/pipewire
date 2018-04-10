audioconvert_sources = ['fmtconvert.c',
			'channelmix.c',
			'resample.c',
			'audioconvert.c',
			'plugin.c']

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
                          include_directories : [spa_inc, spa_libinc],
                          dependencies : speexdsp_dep,
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa/audioconvert/'.format(get_option('libdir')))
