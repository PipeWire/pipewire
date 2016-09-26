pinos_module_spa_headers = [
  'spa-alsa-monitor.h',
  'spa-audiotestsrc.h',
  'spa-v4l2-monitor.h',
  'spa-videotestsrc.h',
]

pinos_module_spa_sources = [
  'module.c',
  'spa-alsa-monitor.c',
  'spa-audiotestsrc.c',
  'spa-v4l2-monitor.c',
  'spa-videotestsrc.c',
]

pinos_module_spa_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]


pinos_module_spa = shared_library('pinos-module-spa', pinos_module_spa_sources,
  c_args : pinos_module_spa_c_args,
  include_directories : [configinc, pinosinc, spa_inc],
  link_with : spalib,
  dependencies : [gobject_dep, gmodule_dep, glib_dep, gio_dep, mathlib, dl_lib, pinos_dep, pinoscore_dep],
)
