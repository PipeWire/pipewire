pkgconf = configuration_data()

pkgconf.set('prefix', get_option('prefix'))
pkgconf.set('exec_prefix', '${prefix}')
pkgconf.set('libdir', '${prefix}/@0@'.format(get_option('libdir')))
pkgconf.set('includedir', '${prefix}/@0@'.format(get_option('includedir')))
pkgconf.set('PIPEWIRE_API_VERSION', apiversion)
pkgconf.set('VERSION', pipewire_version)

pkg_install_dir = '@0@/pkgconfig'.format(get_option('libdir'))

pkg_files = [
  'libpipewire',
  'libpipewirecore',
  'libspa',
]

foreach p : pkg_files
  infile = p + '.pc.in'
  outfile = p + '-0.1.pc'
  configure_file(input : infile,
    output : outfile,
    configuration : pkgconf,
    install_dir : pkg_install_dir)
endforeach
