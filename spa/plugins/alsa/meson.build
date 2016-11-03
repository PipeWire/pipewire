spa_alsa_sources = ['alsa.c',
                'alsa-monitor.c',
                'alsa-sink.c',
                'alsa-source.c',
                'alsa-utils.c']

spa_alsa = shared_library('spa-alsa',
                           spa_alsa_sources,
                           include_directories : [spa_inc, spa_libinc],
                           dependencies : [ alsa_dep, libudev_dep ],
                           link_with : spalib,
                           install : true,
                           install_dir : '@0@/spa'.format(get_option('libdir')))
