subdir('acp')

spa_alsa_sources = ['alsa.c',
                'alsa-udev.c',
                'alsa-acp-device.c',
                'alsa-pcm-device.c',
                'alsa-pcm-sink.c',
                'alsa-pcm-source.c',
                'alsa-pcm.c',
                'alsa-seq-source.c',
                'alsa-seq.c']

spa_alsa = shared_library(
  'spa-alsa',
  [ spa_alsa_sources ],
  c_args : acp_c_args,
  include_directories : [spa_inc, configinc],
  dependencies : [ alsa_dep, libudev_dep, mathlib ],
  link_with : [ acp_lib ],
  install : true,
  install_dir : join_paths(spa_plugindir, 'alsa')
)
