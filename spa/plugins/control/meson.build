control_sources = ['mixer.c',
		   'plugin.c']

controllib = shared_library('spa-control',
                          control_sources,
                          include_directories : [spa_inc],
                          dependencies : [ mathlib ],
                          install : true,
                          install_dir : '@0@/spa/control/'.format(get_option('libdir')))
