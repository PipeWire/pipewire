doxyfile_conf = configuration_data()
doxyfile_conf.set('PACKAGE_NAME', meson.project_name())
doxyfile_conf.set('PACKAGE_VERSION', meson.project_version())
doxyfile_conf.set('top_srcdir', meson.source_root())
doxyfile_conf.set('top_builddir', meson.build_root())

dot_found = find_program('dot', required: false).found()
summary({'dot (used with doxygen)': dot_found}, bool_yn: true, section: 'Optional programs')
if dot_found
  doxyfile_conf.set('HAVE_DOT', 'YES')
else
  doxyfile_conf.set('HAVE_DOT', 'NO')
endif

# Note: order here is how doxygen will expose the pages in the sidebar
# api-tree.dox should be first to determine ordering of Modules.
extra_docs = [
  'api-tree.dox',
  'index.dox',
  'overview.dox',
  'pipewire.dox',
  'pipewire-design.dox',
  'pipewire-access.dox',
  'pipewire-midi.dox',
  'pipewire-portal.dox',
  'pipewire-daemon.dox',
  'pipewire-library.dox',
  'pipewire-modules.dox',
  'pipewire-tools.dox',
  'pipewire-session-manager.dox',
  'pipewire-objects-design.dox',
  'pipewire-audio.dox',
  'tutorial.dox',
  'tutorial1.dox',
  'tutorial2.dox',
  'tutorial3.dox',
  'tutorial4.dox',
  'tutorial5.dox',
  'tutorial6.dox',
  'api.dox',
  'spa-index.dox',
  'spa-plugins.dox',
  'spa-design.dox',
  'spa-pod.dox',
  'spa-buffer.dox',
  'pulseaudio.dox',
  'dma-buf.dox',
]

inputs = []
foreach extra : extra_docs
  inputs += meson.source_root() / 'doc' / extra
endforeach
foreach h : pipewire_headers
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : pipewire_ext_headers
  inputs += meson.source_root() / 'src' / 'pipewire' / 'extensions' / h
endforeach
foreach h : pipewire_ext_sm_headers
  inputs += meson.source_root() / 'src' / 'pipewire' / 'extensions' / h
endforeach
foreach h : pipewire_sources
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : module_sources
  inputs += meson.source_root() / 'src' / 'modules' / h
endforeach
inputs += meson.source_root() / 'test' / 'pwtest.h'
input_dirs = [ meson.source_root() / 'spa' / 'include' / 'spa' ]

path_prefixes = [
  meson.source_root() / 'src',
  meson.source_root() / 'spa' / 'include',
  meson.source_root(),
]

cssfiles = [
  meson.source_root() / 'doc' / 'doxygen-awesome.css',
  meson.source_root() / 'doc' / 'custom.css'
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

doxyfile_conf.set('inputs', ' '.join(inputs + input_dirs))
doxyfile_conf.set('cssfiles', ' '.join(cssfiles))
doxyfile_conf.set('path_prefixes', ' '.join(path_prefixes))
doxyfile_conf.set('c_input_filter', meson.source_root() / 'doc' / 'input-filter.sh')
doxyfile_conf.set('h_input_filter', meson.source_root() / 'doc' / 'input-filter-h.sh')

doxyfile = configure_file(input: 'Doxyfile.in',
                          output: 'Doxyfile',
                          configuration: doxyfile_conf)

docdir = get_option('docdir')
if docdir == ''
  docdir = pipewire_datadir / 'doc' / meson.project_name()
endif

html_target = custom_target('pipewire-docs',
                            input: [ doxyfile, examples_dox ] + inputs + cssfiles,
                            output: [ 'html' ],
                            command: [ doxygen, doxyfile ],
                            install: true,
                            install_dir: docdir)
