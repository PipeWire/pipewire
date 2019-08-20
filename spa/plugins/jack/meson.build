spa_jack_sources = ['plugin.c',
                'jack-client.c',
                'jack-device.c',
                'jack-sink.c',
                'jack-source.c']

spa_jack = shared_library('spa-jack',
                           spa_jack_sources,
                           include_directories : [spa_inc],
                           dependencies : [ jack_dep, mathlib ],
                           install : true,
                           install_dir : '@0@/spa/jack'.format(get_option('libdir')))
