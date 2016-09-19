alsa_sources = ['alsa.c',
                'alsa-monitor.c',
                'alsa-sink.c',
                'alsa-source.c',
                'alsa-utils.c']

alsalib = shared_library('spa-alsa',
                          alsa_sources,
                          include_directories : inc,
                          dependencies : [ alsa_dep, libudev_dep ],
                          link_with : spalib,
                          install : true)
