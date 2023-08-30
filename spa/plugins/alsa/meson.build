subdir('acp')
subdir('mixer')

spa_alsa_dependencies = [ spa_dep, alsa_dep, libudev_dep, mathlib, epoll_shim_dep, libinotify_dep ]

spa_alsa_sources = ['alsa.c',
                'alsa.h',
                'alsa-udev.c',
                'alsa-acp-device.c',
                'alsa-pcm-device.c',
                'alsa-pcm-sink.c',
                'alsa-pcm-source.c',
                'alsa-pcm.c',
                'alsa-seq-bridge.c',
                'alsa-seq.c']

if compress_offload_option.allowed()
  spa_alsa_sources += ['alsa-compress-offload-sink.c',
                       'alsa-compress-offload-device.c',
                       'compress-offload-api-util.c',
                       'compress-offload-api.c']
endif

spa_alsa = shared_library(
  'spa-alsa',
  [ spa_alsa_sources ],
  c_args : acp_c_args,
  include_directories : [configinc],
  dependencies : spa_alsa_dependencies,
  link_with : [ acp_lib ],
  install : true,
  install_dir : spa_plugindir / 'alsa'
)

alsa_udevrules = [
  '90-pipewire-alsa.rules',
]

executable('spa-acp-tool',
  [ 'acp-tool.c' ],
  c_args : acp_c_args,
  dependencies : [ spa_dep, alsa_dep, mathlib, acp_dep ],
  install : true,
)

executable('test-timer',
  [ 'test-timer.c' ],
  dependencies : [ spa_dep, alsa_dep, mathlib, epoll_shim_dep ],
  install : false,
)

executable('test-hw-params',
  [ 'test-hw-params.c' ],
  dependencies : [ spa_dep, alsa_dep, mathlib ],
  install : false,
)

if libudev_dep.found()
  install_data(alsa_udevrules,
    install_dir : udevrulesdir,
  )
endif
