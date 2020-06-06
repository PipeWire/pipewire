pipewire_daemon_sources = [
  'command.c',
  'daemon-config.c',
  'main.c',
]

pipewire_daemon_headers = [
  'command.h',
  'daemon-config.h',
]

pipewire_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DG_LOG_DOMAIN=g_log_domain_pipewire',
]

conf_config = configuration_data()
conf_config.set('VERSION', '"@0@"'.format(pipewire_version))
conf_config.set('media_session_path', pipewire_bindir / 'pipewire-media-session')
conf_install_dir = join_paths(get_option('sysconfdir'), 'pipewire')

conf_config_uninstalled = conf_config
conf_config_uninstalled.set('media_session_path', 'pipewire-media-session')

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire.conf',
               configuration : conf_config,
               install_dir : conf_install_dir)

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire-uninstalled.conf',
               configuration : conf_config)

executable('pipewire',
  pipewire_daemon_sources,
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep, ],
)

if get_option('systemd') and systemd.found()
  subdir('systemd')
endif
