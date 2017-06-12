pipewirecore_headers = [
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
  'protocol-native.h',
  'resource.h',
  'work-queue.h',
]

pipewirecore_sources = [
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
  'protocol-native.c',
  'resource.c',
  'work-queue.c',
]

libpipewirecore_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]


libpipewirecore = shared_library('pipewirecore', pipewirecore_sources,
  version : libversion,
  soversion : soversion,
  c_args : libpipewirecore_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewirecore_dep = declare_dependency(link_with : libpipewirecore,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep],
)
