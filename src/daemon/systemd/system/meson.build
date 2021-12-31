systemd_system_services_dir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir', pkgconfig_define : [ 'rootprefix', prefix])

install_data(sources : 'pipewire.socket',
             install_dir : systemd_system_services_dir)

systemd_config = configuration_data()
systemd_config.set('PW_BINARY', pipewire_bindir / 'pipewire')

configure_file(input : 'pipewire.service.in',
               output : 'pipewire.service',
               configuration : systemd_config,
               install_dir : systemd_system_services_dir)
