audioconvert_sources = ['fmtconvert.c',
			'channelmix.c',
			'resample.c',
			'splitter.c',
			'merger.c',
			'audioconvert.c',
			'plugin.c']

audioconvertlib = shared_library('spa-audioconvert',
                          audioconvert_sources,
                          include_directories : [spa_inc],
                          dependencies : speexdsp_dep,
                          install : true,
                          install_dir : '@0@/spa/audioconvert/'.format(get_option('libdir')))
