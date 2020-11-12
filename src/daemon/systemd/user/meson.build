systemd_user_services_dir = systemd.get_pkgconfig_variable('systemduserunitdir', define_variable : [ 'prefix', prefix])

install_data(
  sources : ['pipewire.socket', 'pipewire-pulse.socket'],
  install_dir : systemd_user_services_dir)

systemd_config = configuration_data()
systemd_config.set('PW_BINARY', join_paths(pipewire_bindir, 'pipewire'))
systemd_config.set('PW_PULSE_BINARY', join_paths(pipewire_bindir, 'pipewire-pulse'))

configure_file(input : 'pipewire.service.in',
               output : 'pipewire.service',
               configuration : systemd_config,
               install_dir : systemd_user_services_dir)

configure_file(input : 'pipewire-pulse.service.in',
               output : 'pipewire-pulse.service',
               configuration : systemd_config,
               install_dir : systemd_user_services_dir)
