spa_alsa_sources = ['alsa.c',
                'alsa-udev.c',
                'alsa-pcm-device.c',
                'alsa-pcm-sink.c',
                'alsa-pcm-source.c',
                'alsa-pcm.c',
                'alsa-seq-source.c',
                'alsa-seq.c']

spa_alsa = shared_library('spa-alsa',
                           spa_alsa_sources,
                           include_directories : [spa_inc],
                           dependencies : [ alsa_dep, libudev_dep, mathlib, ],
                           install : true,
                           install_dir : join_paths(spa_plugindir, 'alsa'))
