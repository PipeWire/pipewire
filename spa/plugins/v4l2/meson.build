v4l2_sources = ['v4l2.c',
                'v4l2-source.c']

v4l2lib = shared_library('spa-v4l2',
                          v4l2_sources,
                          include_directories : inc,
                          dependencies : v4l2_dep,
                          link_with : spalib,
                          install : true)
