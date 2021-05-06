doxyfile_conf = configuration_data()
doxyfile_conf.set('PACKAGE_NAME', meson.project_name())
doxyfile_conf.set('PACKAGE_VERSION', meson.project_version())
doxyfile_conf.set('top_srcdir', meson.source_root())
doxyfile_conf.set('top_builddir', meson.build_root())

if find_program('dot', required: false).found()
  doxyfile_conf.set('HAVE_DOT', 'YES')
else
  doxyfile_conf.set('HAVE_DOT', 'NO')
endif

inputs = []
foreach h : pipewire_headers
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : pipewire_sources
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach

extra_docs = [
  'overview.md',
  'design.txt',
  'architecture.md',
  'objects_design.md',
  'tutorial-index.md',
  'tutorial1.md',
  'tutorial2.md',
  'tutorial3.md',
  'tutorial4.md',
  'tutorial5.md',
  'tutorial6.md',
  'spa/index.md',
  'spa/design.md',
  'spa/pod.md',
  'spa/buffer.md',
  'pulseaudio.md',
]

foreach extra : extra_docs
  inputs += meson.source_root() / 'doc' / extra
endforeach

doxyfile_conf.set('inputs', ' '.join(inputs))

doxyfile = configure_file(input: 'Doxyfile.in',
                          output: 'Doxyfile',
                          configuration: doxyfile_conf)

docdir = get_option('docdir')
if docdir == ''
  docdir = pipewire_datadir / 'doc' / meson.project_name()
endif

html_target = custom_target('pipewire-docs',
                            input: [ doxyfile ] + inputs,
                            output: [ 'html' ],
                            command: [ doxygen, doxyfile ],
                            install: true,
                            install_dir: docdir)
