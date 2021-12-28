control_sources = [
  'mixer.c',
  'plugin.c'
]

controllib = shared_library('spa-control',
  control_sources,
  dependencies : [ spa_dep, mathlib ],
  install : true,
  install_dir : spa_plugindir / 'control')
