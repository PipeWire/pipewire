pinos_module_spa_headers = [
  'spa-node.h',
  'spa-monitor.h',
]

pinos_module_spa_sources = [
  'module.c',
  'spa-node.c',
  'spa-monitor.c',
]

pinos_module_spa_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]

pinos_module_spa = shared_library('pinos-module-spa', pinos_module_spa_sources,
  c_args : pinos_module_spa_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-0.1'.format(get_option('libdir')),
  dependencies : [mathlib, dl_lib, pinos_dep, pinoscore_dep],
)
