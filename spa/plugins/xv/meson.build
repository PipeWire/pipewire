xv_sources = ['xv.c',
              'xv-sink.c']

xvlib = shared_library('spa-xv',
                       xv_sources,
                       include_directories : inc,
                       dependencies : xv_dep,
                       link_with : spalib,
                       install : true)
