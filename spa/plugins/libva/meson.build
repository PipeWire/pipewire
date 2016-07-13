libva_sources = ['libva.c',
                 'libva-dec.c',
                 'libva-enc.c']

libvalib = shared_library('spa-libva',
                          libva_sources,
                          include_directories : inc,
                          dependencies : libva_dep,
                          link_with : spalib,
                          install : true)
