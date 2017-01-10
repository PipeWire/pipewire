pinoscore_headers = [
  'access.h',
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
  'resource.h',
]

pinoscore_sources = [
  'access.c',
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
  'resource.c',
]

libpinoscore_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]


libpinoscore = shared_library('pinoscore', pinoscore_sources,
  version : libversion,
  soversion : soversion,
  c_args : libpinoscore_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [mathlib, dl_lib, pinos_dep],
)

pinoscore_dep = declare_dependency(link_with : libpinoscore,
  include_directories : [configinc, spa_inc],
  dependencies : [pinos_dep],
)
