videoconvert_sources = [
  'videoadapter.c',
  'plugin.c'
]

simd_cargs = []
simd_dependencies = []

videoconvertlib = shared_library('spa-videoconvert',
  videoconvert_sources,
  c_args : simd_cargs,
  dependencies : [ spa_dep, mathlib ],
  link_with : simd_dependencies,
  install : true,
  install_dir : spa_plugindir / 'videoconvert')
