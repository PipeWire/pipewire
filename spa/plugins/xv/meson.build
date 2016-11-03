xv_sources = ['xv.c',
              'xv-sink.c']

xvlib = shared_library('spa-xv',
                       xv_sources,
                       include_directories : [spa_inc, spa_libinc],
                       dependencies : xv_dep,
                       link_with : spalib,
                       install : true,
                       install_dir : '@0@/spa'.format(get_option('libdir')))
