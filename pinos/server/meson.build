pinoscore_headers = [
  'client.h',
  'client-node.h',
  'command.h',
  'core.h',
  'data-loop.h',
  'link.h',
  'main-loop.h',
  'module.h',
  'node.h',
  'node-factory.h',
  'port.h',
  'registry.h',
]

pinoscore_sources = [
  'client.c',
  'client-node.c',
  'command.c',
  'core.c',
  'data-loop.c',
  'link.c',
  'main-loop.c',
  'module.c',
  'node.c',
  'node-factory.c',
  'port.c',
  'registry.c',
]

libpinoscore_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]


libpinoscore = shared_library('pinoscore', pinoscore_sources, gdbus_target,
  version : libversion,
  soversion : soversion,
  c_args : libpinoscore_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [gobject_dep, gmodule_dep, glib_dep, gio_dep, mathlib, dl_lib, pinos_dep],
)

pinoscore_dep = declare_dependency(link_with : libpinoscore,
  include_directories : [configinc, spa_inc],
  dependencies : [glib_dep, gobject_dep, gmodule_dep, pinos_dep],
)
