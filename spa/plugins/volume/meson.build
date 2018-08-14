volume_sources = ['volume.c', 'plugin.c']

volumelib = shared_library('spa-volume',
                           volume_sources,
                           include_directories : [spa_inc],
                           install : true,
                           install_dir : '@0@/spa/volume'.format(get_option('libdir')))
