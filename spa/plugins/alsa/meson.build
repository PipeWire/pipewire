alsa_sources = ['alsa.c',
                'alsa-sink.c']

alsalib = shared_library('spa-alsa',
                          alsa_sources,
                          include_directories : inc,
                          dependencies : alsa_dep,
                          link_with : spalib,
                          install : true)
