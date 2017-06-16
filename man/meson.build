manpage_conf = configuration_data()
manpage_conf.set('PACKAGE_NAME', meson.project_name())
manpage_conf.set('PACKAGE_VERSION', meson.project_version())
manpage_conf.set('PACKAGE_URL', 'http://pipewire.org')
manpage_conf.set('top_srcdir', meson.source_root())
manpage_conf.set('top_builddir', meson.build_root())

manpages = ['pipewire.1',
	    'pipewire-monitor.1' ]

foreach m : manpages
  infile = m + '.xml.in'
  outfile = m + '.xml'
  xml = configure_file(input : infile,
		       output : outfile,
		       configuration : manpage_conf)
  custom_target(m + '.target',
		output : m,
		input : xml,
		command : [xmltoman, '@INPUT@'],
		capture : true,
		install : true,
		install_dir : join_paths(get_option('mandir'), 'man1'))
endforeach
