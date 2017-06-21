pipewire_headers = [
  'array.h',
  'connection.h',
  'context.h',
  'extension.h',
  'interfaces.h',
  'introspect.h',
  'log.h',
  'loop.h',
  'map.h',
  'mem.h',
  'pipewire.h',
  'properties.h',
  'protocol.h',
  'proxy.h',
  'rtkit.h',
  'sig.h',
  'stream.h',
  'subscribe.h',
  'thread-loop.h',
  'transport.h',
  'type.h',
  'utils.h',
]

pipewire_sources = [
  'connection.c',
  'context.c',
  'extension.c',
  'introspect.c',
  'log.c',
  'loop.c',
  'mem.c',
  'pipewire.c',
  'properties.c',
  'protocol.c',
  'proxy.c',
  'stream.c',
  'rtkit.c',
  'thread-loop.c',
  'transport.c',
  'type.c',
  'utils.c',
]

install_headers(pipewire_headers, subdir : 'pipewire/client')

libpipewire_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-D_POSIX_C_SOURCE',
]


libpipewire = shared_library('pipewire-@0@'.format(apiversion), pipewire_sources,
  version : libversion,
  soversion : soversion,
  c_args : libpipewire_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [dbus_dep, dl_lib, mathlib, pthread_lib],
)

pipewire_dep = declare_dependency(link_with : libpipewire,
  include_directories : [configinc, spa_inc],
  dependencies : [pthread_lib],
)
