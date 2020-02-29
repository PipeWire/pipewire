systemd_user_services_dir = systemd.get_pkgconfig_variable('systemduserunitdir', define_variable : [ 'prefix', prefix])

install_data(sources : 'pipewire.socket', install_dir : systemd_user_services_dir)

systemd_config = configuration_data()
systemd_config.set('PW_BINARY', join_paths(pipewire_bindir, 'pipewire'))

configure_file(input : 'pipewire.service.in',
               output : 'pipewire.service',
               configuration : systemd_config,
               install_dir : systemd_user_services_dir)
