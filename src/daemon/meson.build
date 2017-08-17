pipewire_sources = [
  'main.c',
  'daemon-config.c',
]

pipewire_headers = [
  'daemon-config.h',
]

pipewire_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DG_LOG_DOMAIN=g_log_domain_pipewire',
]

conf_config = configuration_data()
conf_install_dir = '@0@/pipewire'.format(get_option('sysconfdir'))

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire.conf',
               configuration : conf_config,
               install_dir : conf_install_dir)

executable('pipewire',
  pipewire_sources,
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep],
)

subdir('systemd')
