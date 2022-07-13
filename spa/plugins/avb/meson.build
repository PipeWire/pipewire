spa_avb_sources = ['avb.c',
                'avb.h',
                'avb-pcm-sink.c',
                'avb-pcm-source.c',
                'avb-pcm.c' ]

spa_avb = shared_library(
  'spa-avb',
  [ spa_avb_sources ],
  include_directories : [configinc],
  dependencies : [ spa_dep, mathlib, epoll_shim_dep ],
  install : true,
  install_dir : spa_plugindir / 'avb'
)
