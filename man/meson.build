manpage_conf = configuration_data()
manpage_conf.set('PACKAGE_NAME', meson.project_name())
manpage_conf.set('PACKAGE_VERSION', meson.project_version())
manpage_conf.set('PACKAGE_URL', 'http://pipewire.org')
manpage_conf.set('PACKAGE_BUGREPORT', 'https://gitlab.freedesktop.org/pipewire/pipewire/issues')
manpage_conf.set('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
manpage_conf.set('top_srcdir', meson.source_root())
manpage_conf.set('top_builddir', meson.build_root())

manpages = [
  [ 'pipewire', '1' ],
  [ 'pipewire.conf', '5' ],
  [ 'pw-cli', '1' ],
  [ 'pw-mon', '1' ]
]

foreach m : manpages
  file = m.get(0) + '.' + m.get(1)
  infile = file + '.xml.in'
  outfile = file + '.xml'
  xml = configure_file(input : infile,
		       output : outfile,
		       configuration : manpage_conf)
  custom_target(file + '.target',
		output : file,
		input : xml,
		command : [xmltoman, '@INPUT@'],
		capture : true,
		install : true,
		install_dir : join_paths(get_option('mandir'), 'man' + m.get(1)))
endforeach
