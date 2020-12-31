pipewire_daemon_sources = [
  'main.c',
]

pipewire_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DG_LOG_DOMAIN=g_log_domain_pipewire',
]

conf_config = configuration_data()
conf_config.set('VERSION', '"@0@"'.format(pipewire_version))
conf_config.set('media_session_path', join_paths(pipewire_bindir, 'pipewire-media-session'))
conf_config.set('pipewire_pulse_path', join_paths(pipewire_bindir, 'pipewire-pulse'))
conf_install_dir = join_paths(get_option('sysconfdir'), 'pipewire')

install_data(
  sources : [
    'media-session.d/with-jack',
    'media-session.d/with-pulseaudio',
    'media-session.d/media-session.conf',
    'media-session.d/alsa-monitor.conf' ],
  install_dir : join_paths(conf_install_dir, 'media-session.d'))

conf_config_uninstalled = conf_config
conf_config_uninstalled.set('media_session_path',
  join_paths(meson.build_root(), 'src', 'examples', 'pipewire-media-session'))
conf_config_uninstalled.set('pipewire_pulse_path',
  join_paths(meson.build_root(), 'src', 'daemon', 'pipewire-pulse'))

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire.conf',
               configuration : conf_config,
               install_dir : conf_install_dir)

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire-uninstalled.conf',
               configuration : conf_config_uninstalled)

executable('pipewire',
  pipewire_daemon_sources,
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep, ],
)

executable('pipewire-pulse',
  'pipewire-pulse.c',
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep, ],
)

if get_option('systemd') and systemd.found()
  subdir('systemd')
endif
