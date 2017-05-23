pipewire_module_spa_headers = [
  'spa-node.h',
  'spa-monitor.h',
]

pipewire_module_spa_sources = [
  'module.c',
  'spa-node.c',
  'spa-monitor.c',
]

pipewire_module_spa_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]

pipewire_module_spa = shared_library('pipewire-module-spa', pipewire_module_spa_sources,
  c_args : pipewire_module_spa_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pipewire-0.1'.format(get_option('libdir')),
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)
