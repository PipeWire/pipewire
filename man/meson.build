manpage_conf = configuration_data()
manpage_conf.set('PACKAGE_NAME', meson.project_name())
manpage_conf.set('PACKAGE_VERSION', meson.project_version())
manpage_conf.set('PACKAGE_URL', 'https://pipewire.org')
manpage_conf.set('PACKAGE_BUGREPORT', 'https://gitlab.freedesktop.org/pipewire/pipewire/issues')
manpage_conf.set('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
manpage_conf.set('PIPEWIRE_CONFDATADIR', pipewire_confdatadir)

manpages = [
  'pipewire.1.rst.in',
  'pipewire-pulse.1.rst.in',
  'pipewire.conf.5.rst.in',
  'pw-cat.1.rst.in',
  'pw-cli.1.rst.in',
  'pw-config.1.rst.in',
  'pw-dot.1.rst.in',
  'pw-link.1.rst.in',
  'pw-metadata.1.rst.in',
  'pw-mididump.1.rst.in',
  'pw-mon.1.rst.in',
  'pw-profiler.1.rst.in',
  'pw-top.1.rst.in',
]

if get_option('pipewire-jack').allowed()
  manpages += 'pw-jack.1.rst.in'
endif

if not generate_manpages
  subdir_done()
endif

foreach m : manpages
  file = m.split('.rst.in').get(0)
  rst = configure_file(input : m,
                       output : file + '.rst',
                       configuration : manpage_conf)
  section = file.split('.').get(-1)
  custom_target(file + '.target',
                output : file,
                input : rst,
                command : [rst2man, '@INPUT@', '@OUTPUT@'],
                install : true,
                install_dir : get_option('mandir') / 'man' + section)
endforeach
