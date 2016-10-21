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

conf_config = configuration_data()
conf_install_dir = '@0@/pinos'.format(get_option('sysconfdir'))

configure_file(input : 'pinos.conf.in',
               output : 'pinos.conf',
               configuration : conf_config,
               install_dir : conf_install_dir)

executable('pinos',
  pinos_sources,
  install: true,
  c_args : pinos_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pinos_dep, pinoscore_dep],
)
