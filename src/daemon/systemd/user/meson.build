systemd_install_dir = '@0@/systemd/user'.format(get_option('libdir'))

pipewire_systemd_files = [
  'pipewire.socket',
]

install_data(pipewire_systemd_files, install_dir : systemd_install_dir)

systemd_config = configuration_data()
systemd_config.set('PW_BINARY', '@0@/pipewire'.format(pipewire_bindir))

configure_file(input : 'pipewire.service.in',
               output : 'pipewire.service',
               configuration : systemd_config,
               install_dir : systemd_install_dir)
