pinos_sources = [
  'main.c',
  'daemon-config.c',
]

pinos_headers = [
  'daemon-config.h',
]

pinos_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DG_LOG_DOMAIN=g_log_domain_pinos',
]

executable('pinos',
  pinos_sources,
  install: true,
  c_args : pinos_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [glib_dep, gobject_dep, gmodule_dep, pinos_dep, pinoscore_dep],
)
