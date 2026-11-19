sndio_sources = [
  'plugin.c',
  'sndio.c',
  'sndio-sink.c',
  'sndio-source.c',
  'sndio-device.c',
]

sndiolib = shared_library('spa-sndio',
  sndio_sources,
  dependencies : [ spa_dep, sndio_dep, mathlib, pthread_lib ],
  install : true,
  install_dir : spa_plugindir / 'sndio')
