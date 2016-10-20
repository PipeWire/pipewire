pinos_headers = [
  'context.h',
  'format.h',
  'introspect.h',
  'pinos.h',
  'properties.h',
  'stream.h',
  'ringbuffer.h',
  'subscribe.h',
  'thread-mainloop.h',
]

pinos_sources = [
  'connection.c',
  'context.c',
  'format.c',
  'introspect.c',
  'properties.c',
  'serialize.c',
  'stream.c',
  'pinos.c',
  'ringbuffer.c',
  'subscribe.c',
  'thread-mainloop.c',
  gdbus_target,
]

install_headers(pinos_headers, subdir : 'pinos/client')

mkenums = find_program('build_mkenum.py')
glib_mkenums = find_program('glib-mkenums')

enumtypes_h = custom_target('enumtypes_h',
  output : 'enumtypes.h',
  input : pinos_headers,
  install : true,
  install_dir : 'include/pinos/client',
  command : [mkenums, glib_mkenums, '@OUTPUT@', '@INPUT@'])

enumtypes_c = custom_target('enumtypes_c',
  output : 'enumtypes.c',
  input : pinos_headers,
  depends : [enumtypes_h],
  command : [mkenums, glib_mkenums, '@OUTPUT@', '@INPUT@'])


libpinos_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]


pinos_gen_sources = [enumtypes_h]

libpinos = shared_library('pinos', pinos_sources,
  enumtypes_h, enumtypes_c,
  version : libversion,
  soversion : soversion,
  c_args : libpinos_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  dependencies : [gobject_dep, gmodule_dep, glib_dep, gio_dep, mathlib],
)

pinos_dep = declare_dependency(link_with : libpinos,
  include_directories : [configinc, spa_inc],
  dependencies : [glib_dep, gobject_dep, gmodule_dep],
  # Everything that uses libpinos needs this built to compile
  sources : pinos_gen_sources,
)
