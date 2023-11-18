doxyfile_conf = configuration_data()
doxyfile_conf.set('PACKAGE_NAME', meson.project_name())
doxyfile_conf.set('PACKAGE_VERSION', meson.project_version())
doxyfile_conf.set('top_srcdir', meson.project_source_root())
doxyfile_conf.set('top_builddir', meson.project_build_root())
doxyfile_conf.set('output_directory', meson.current_build_dir())

dot_found = find_program('dot', required: false).found()
summary({'dot (used with doxygen)': dot_found}, bool_yn: true, section: 'Optional programs')
if dot_found
  doxyfile_conf.set('HAVE_DOT', 'YES')
else
  doxyfile_conf.set('HAVE_DOT', 'NO')
endif

# Note: order here is how doxygen will expose the pages in the sidebar
# tree.dox should be first to determine the ordering.
extra_docs = [
  'tree.dox',
  'dox/index.dox',
  'dox/overview.dox',
  'dox/modules.dox',
  'dox/internals/index.dox',
  'dox/internals/design.dox',
  'dox/internals/access.dox',
  'dox/internals/midi.dox',
  'dox/internals/portal.dox',
  'dox/internals/daemon.dox',
  'dox/internals/library.dox',
  'dox/internals/session-manager.dox',
  'dox/internals/objects.dox',
  'dox/internals/audio.dox',
  'dox/internals/scheduling.dox',
  'dox/internals/protocol.dox',
  'dox/internals/pulseaudio.dox',
  'dox/internals/dma-buf.dox',
  'dox/tutorial/index.dox',
  'dox/tutorial/tutorial1.dox',
  'dox/tutorial/tutorial2.dox',
  'dox/tutorial/tutorial3.dox',
  'dox/tutorial/tutorial4.dox',
  'dox/tutorial/tutorial5.dox',
  'dox/tutorial/tutorial6.dox',
  'dox/api/index.dox',
  'dox/api/spa-index.dox',
  'dox/api/spa-plugins.dox',
  'dox/api/spa-design.dox',
  'dox/api/spa-pod.dox',
  'dox/api/spa-buffer.dox',
]

inputs = []
foreach extra : extra_docs
  inputs += meson.project_source_root() / 'doc' / extra
endforeach
foreach h : pipewire_headers
  inputs += meson.project_source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : pipewire_ext_headers
  inputs += meson.project_source_root() / 'src' / 'pipewire' / 'extensions' / h
endforeach
foreach h : pipewire_ext_sm_headers
  inputs += meson.project_source_root() / 'src' / 'pipewire' / 'extensions' / h
endforeach
foreach h : pipewire_sources
  inputs += meson.project_source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : module_sources
  inputs += meson.project_source_root() / 'src' / 'modules' / h
endforeach
input_dirs = [ meson.project_source_root() / 'spa' / 'include' / 'spa' ]

path_prefixes = [
  meson.project_source_root() / 'src',
  meson.project_source_root() / 'spa' / 'include',
  meson.project_source_root(),
]

cssfiles = [
  meson.project_source_root() / 'doc' / 'doxygen-awesome.css',
  meson.project_source_root() / 'doc' / 'custom.css'
]

# Example files (in order from simple to esoteric)
example_files = [
  'tutorial1.c',
  'tutorial2.c',
  'tutorial3.c',
  'tutorial4.c',
  'tutorial5.c',
  'tutorial6.c',
]
foreach h : examples
  example_files += [h + '.c']
endforeach
foreach h : spa_examples
  example_files += ['spa/examples/' + h + '.c']
endforeach

example_doxygen = []
example_ref = []
foreach h : example_files
  example_doxygen += ['\\example ' + h,
                      '\\snippet{doc} ' + h + ' title',
                      '<br>',
                      '\\snippet{doc} ' + h + ' doc']
  example_ref += ['- \\ref ' + h + ' "": \snippet{doc} ' + h + ' title']
endforeach

examples_dox_conf = configuration_data()
examples_dox_conf.set('example_doxygen', '\n'.join(example_doxygen))
examples_dox_conf.set('example_ref', '\n'.join(example_ref))
examples_dox = configure_file(input: 'examples.dox.in',
                              output: 'examples.dox',
                              configuration: examples_dox_conf)

input_dirs += [ 'doc/examples.dox' ]

man_doxygen = []
man_subpages = []
foreach m : manpages
  manconf = configuration_data()
  pagename = 'page_man_' + m.split('.rst.in').get(0).replace('.', '_').replace('-', '_')
  filename = m.split('.rst.in').get(0) + '.dox'
  manconf.set('pagename', pagename)
  manconf.set('title', m.split('.rst.in').get(0).replace('.1','').replace('.5',''))
  manconf.set('filename', meson.project_source_root() / 'man' / m)
  manfile = configure_file(input: 'manpage.dox.in',
                           output: filename,
                           configuration: manconf)
  man_doxygen += [manfile]
  man_subpages += ['- \subpage ' + pagename]
  input_dirs += [ 'doc/' + filename ]
endforeach

pw_programs_dox_conf = configuration_data()
pw_programs_dox_conf.set('man_subpages', '\n'.join(man_subpages))
pw_programs_dox = configure_file(input: 'programs.dox.in',
                          output: 'programs.dox',
                          configuration: pw_programs_dox_conf)
input_dirs += [ 'doc/programs.dox' ]

doxygen_layout = meson.project_source_root() / 'doc' / 'DoxygenLayout.xml'
doxygen_filter_c = meson.project_source_root() / 'doc' / 'input-filter.sh'
doxygen_filter_h = meson.project_source_root() / 'doc' / 'input-filter-h.sh'

doxyfile_conf.set('inputs', ' '.join(inputs + input_dirs))
doxyfile_conf.set('cssfiles', ' '.join(cssfiles))
doxyfile_conf.set('layout', doxygen_layout)
doxyfile_conf.set('path_prefixes', ' '.join(path_prefixes))
doxyfile_conf.set('c_input_filter', doxygen_filter_c)
doxyfile_conf.set('h_input_filter', doxygen_filter_h)

doxyfile = configure_file(input: 'Doxyfile.in',
                          output: 'Doxyfile',
                          configuration: doxyfile_conf)

docdir = get_option('docdir')
if docdir == ''
  docdir = pipewire_datadir / 'doc' / meson.project_name()
endif

html_target = custom_target('pipewire-docs',
                            input: [ doxyfile, doxygen_layout, examples_dox, pw_programs_dox, doxygen_filter_c, doxygen_filter_h ] + inputs + cssfiles + man_doxygen,
                            output: [ 'html' ],
                            command: [ doxygen, doxyfile ],
                            install: true,
                            install_dir: docdir)


if generate_module_manpages
   module_man_rst_py = meson.project_source_root() / 'doc' / 'module-man-rst.py'
   module_man_defines = []
   foreach m : manpage_conf.keys()
     if m != 'LIBPIPEWIRE_MODULES'
       module_man_defines += ['-D', m, manpage_conf.get(m)]
     endif
   endforeach

   foreach m : module_sources
       name = m.split('.c').get(0)
       file = 'libpipewire-' + name + '.7'

       rst = custom_target(file + '.rst',
                command : [python, module_man_rst_py, pandoc, name, '@INPUT@' ] + module_man_defines,
                input : [ html_target ],
                depend_files : [ module_man_rst_py ],
                output : file + '.rst',
                capture : true
       )
       custom_target(file,
                output : file,
                input : rst,
                command : [rst2man, '@INPUT@', '@OUTPUT@'],
                install : true,
                install_dir : get_option('mandir') / 'man7')
   endforeach
endif
