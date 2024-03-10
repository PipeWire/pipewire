fs = import('fs')

doxyfile_conf = configuration_data()
doxyfile_conf.set('PACKAGE_NAME', meson.project_name())
doxyfile_conf.set('PACKAGE_VERSION', meson.project_version())
doxyfile_conf.set('top_srcdir', meson.project_source_root())
doxyfile_conf.set('top_builddir', meson.project_build_root())
doxyfile_conf.set('output_directory', meson.current_build_dir())

doc_prefix_value = get_option('doc-prefix-value')
doc_sysconfdir_value = get_option('doc-sysconfdir-value')

if doc_prefix_value == '' and doc_sysconfdir_value == ''
   doc_spa_plugindir = spa_plugindir
   doc_pipewire_configdir = pipewire_configdir
   doc_pipewire_confdatadir = pipewire_confdatadir
else
   if doc_prefix_value == ''
     doc_prefix_value = get_option('prefix')
   endif
   if doc_sysconfdir_value == ''
     doc_sysconfdir_value = get_option('sysconfdir')
   endif
   doc_spa_plugindir = doc_prefix_value / get_option('libdir') / spa_name
   doc_pipewire_configdir = doc_prefix_value / doc_sysconfdir_value / 'pipewire'
   doc_pipewire_confdatadir = doc_prefix_value / get_option('datadir') / 'pipewire'
endif

doxygen_env = environment()
doxygen_env.set('PACKAGE_NAME', meson.project_name())
doxygen_env.set('PACKAGE_VERSION', meson.project_version())
doxygen_env.set('PACKAGE_URL', 'https://pipewire.org')
doxygen_env.set('PACKAGE_BUGREPORT', 'https://gitlab.freedesktop.org/pipewire/pipewire/issues')
doxygen_env.set('PIPEWIRE_CONFIG_DIR', doc_pipewire_configdir)
doxygen_env.set('PIPEWIRE_CONFDATADIR', doc_pipewire_confdatadir)
doxygen_env.set('SPA_PLUGINDIR', doc_spa_plugindir)
doxygen_env.set('BUILD_DIR', meson.current_build_dir())

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
  'dox/pulse-modules.dox',
  'dox/programs/index.md',
  'dox/config/index.md',
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

manpage_docs = [
  'dox/config/pipewire-pulse.conf.5.md',
  'dox/config/pipewire.conf.5.md',
  'dox/config/pipewire-client.conf.5.md',
  'dox/config/pipewire-jack.conf.5.md',
  'dox/config/pipewire-devices.7.md',
  'dox/config/pipewire-filter-chain.conf.5.md',
  'dox/config/pipewire-pulse-modules.7.md',
  'dox/config/libpipewire-modules.7.md',
  'dox/programs/pipewire-pulse.1.md',
  'dox/programs/pipewire.1.md',
  'dox/programs/pw-cat.1.md',
  'dox/programs/pw-cli.1.md',
  'dox/programs/pw-config.1.md',
  'dox/programs/pw-dot.1.md',
  'dox/programs/pw-dump.1.md',
  'dox/programs/pw-jack.1.md',
  'dox/programs/pw-link.1.md',
  'dox/programs/pw-loopback.1.md',
  'dox/programs/pw-metadata.1.md',
  'dox/programs/pw-mididump.1.md',
  'dox/programs/pw-mon.1.md',
  'dox/programs/pw-profiler.1.md',
  'dox/programs/pw-reserve.1.md',
  'dox/programs/pw-top.1.md',
  'dox/programs/pw-v4l2.1.md',
  'dox/programs/spa-acp-tool.1.md',
  'dox/programs/spa-inspect.1.md',
  'dox/programs/spa-json-dump.1.md',
  'dox/programs/spa-monitor.1.md',
  'dox/programs/spa-resample.1.md',
]

manpages = []

foreach m : manpage_docs
  name = fs.stem(fs.name(m))
  pagepart = name.replace('.', '_')
  manpages += [[name, f'page_man_@pagepart@']]
  extra_docs += m
endforeach

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
foreach h : pipewire_module_protocol_pulse_sources
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
example_dep_files = []
foreach h : example_files
  example_dep_files += ['examples/' + h]
endforeach
foreach h : examples
  example_files += [h + '.c']
  example_dep_files += ['../src/examples/' + h + '.c']
endforeach
foreach h : spa_examples
  example_files += ['spa/examples/' + h + '.c']
  example_dep_files += ['../spa/examples/' + h + '.c']
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

module_manpage_list = []
foreach m : module_sources
   name = fs.stem(m)
   pagepart = name.replace('-', '_')
   module_manpage_list += f'\\ref page_@pagepart@ "libpipewire-@name@(7)"'
   manpages += [[f'libpipewire-@name@.7', f'page_@pagepart@']]
endforeach

doxygen_env.set('LIBPIPEWIRE_MODULES', '<ul><li>' + '</li><li>'.join(module_manpage_list) + '</li></ul>')

pulse_module_manpage_list = []
foreach m : pipewire_module_protocol_pulse_sources
  name = fs.stem(fs.name(m))
  if m.contains('/modules/') and name.startswith('module-')
    pagepart = name.replace('-', '_')
    pulse_module_manpage_list += f'\\ref page_pulse_@pagepart@ "pipewire-pulse-@name@(7)"'
    manpages += [[f'pipewire-pulse-@name@.7', f'page_pulse_@pagepart@']]
  endif
endforeach

doxygen_env.set('PIPEWIRE_PULSE_MODULES', '<ul><li>' + '</li><li>'.join(pulse_module_manpage_list) + '</li></ul>')

doxygen_layout = meson.project_source_root() / 'doc' / 'DoxygenLayout.xml'
doxygen_filter_c = meson.project_source_root() / 'doc' / 'input-filter.py'
doxygen_filter_h = meson.project_source_root() / 'doc' / 'input-filter-h.sh'
doxygen_filter_md = meson.project_source_root() / 'doc' / 'input-filter-md.py'

doxyfile_conf.set('inputs', ' '.join(inputs + input_dirs))
doxyfile_conf.set('cssfiles', ' '.join(cssfiles))
doxyfile_conf.set('layout', doxygen_layout)
doxyfile_conf.set('path_prefixes', ' '.join(path_prefixes))
doxyfile_conf.set('c_input_filter', doxygen_filter_c)
doxyfile_conf.set('h_input_filter', doxygen_filter_h)
doxyfile_conf.set('md_input_filter', doxygen_filter_md)

doxyfile = configure_file(input: 'Doxyfile.in',
                          output: 'Doxyfile',
                          configuration: doxyfile_conf)

docdir = get_option('docdir')
if docdir == ''
  docdir = pipewire_datadir / 'doc' / meson.project_name()
endif

index_json = custom_target('index.json',
  command: [ doxygen_filter_md, '--index', '@INPUT@' ],
  input: extra_docs + manpage_docs,
  output: 'index.json',
  env: doxygen_env
)

html_target = custom_target('pipewire-docs',
                            input: [ doxyfile, doxygen_layout, example_dep_files, examples_dox,
                                     doxygen_filter_c, doxygen_filter_h, index_json ] + inputs + cssfiles,
                            output: [ 'html' ],
                            command: [ doxygen, doxyfile ],
                            env: doxygen_env,
                            install: install_docs,
                            install_tag: 'doc',
                            install_dir: docdir)


man_fixup = files('man-fixup.py')

manfiles = []

foreach m : manpages
  file = m.get(0)
  page = m.get(1)
  name = fs.stem(file)
  section = file.split('.').get(-1)

  manfiles += custom_target(file,
     command : [ python, man_fixup, '@INPUT@', page, name, section, meson.project_version() ],
     output : file,
     input : html_target,
     depend_files : [ man_fixup ],
     capture : true,
     install : install_man,
     install_tag: 'man',
     install_dir : get_option('mandir') / 'man' + section
  )
endforeach
