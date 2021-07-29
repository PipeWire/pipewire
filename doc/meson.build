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

inputs = []
foreach h : pipewire_headers
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : pipewire_sources
  inputs += meson.source_root() / 'src' / 'pipewire' / h
endforeach
foreach h : module_sources
  inputs += meson.source_root() / 'src' / 'modules' / h
endforeach
foreach h : media_session_sources
  inputs += meson.source_root() / 'src' / 'examples' / h
endforeach
inputs += meson.source_root() / 'test' / 'pwtest.h'

# SPA headers use static inline functions. Doxygen doesn't extract those
# unless we have EXTRACT_STATIC set - but we don't want it to extract
# everything in the rest of the tree.
# The shell script here basically does a:
#   cp spa/* builddir/spa/ && sed -i 's/^static//' buildir/spa/**.h
# The copied files are passed to doxygen as input and they are parsed as
# normal functions.
# Because this uses globbing, this target won't rebuild if the headers
# change but meh.
sed = find_program('sed', required: false)
summary({'sed (for SPA docs)': sed.found()}, bool_yn: true, section: 'Optional programs')
if sed.found()
  spa_srcdir = meson.source_root() / 'spa' / 'include' / 'spa'
  spa_dstdir = meson.current_build_dir() / 'spa'
  spa_strip_static = custom_target(
    'spa-strip-static',
    command: [ find_program('strip-static.sh'), spa_srcdir, spa_dstdir ],
    build_by_default: true,
    output: 'spa',
  )
else
  spa_strip_static = []
endif

# Note: order here is how doxygen will expose the pages in the sidebar
extra_docs = [
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
  'media-session.dox',
  'api.dox',
  'tutorial.dox',
  'tutorial1.dox',
  'tutorial2.dox',
  'tutorial3.dox',
  'tutorial4.dox',
  'tutorial5.dox',
  'tutorial6.dox',
  'spa-index.dox',
  'spa-plugins.dox',
  'spa-design.dox',
  'spa-pod.dox',
  'spa-buffer.dox',
  'pulseaudio.dox',
  'dma-buf.dox',
]

foreach extra : extra_docs
  inputs += meson.source_root() / 'doc' / extra
endforeach

cssfiles = [
  meson.source_root() / 'doc' / 'doxygen-awesome.css',
  meson.source_root() / 'doc' / 'custom.css'
]

doxyfile_conf.set('inputs', ' '.join(inputs + [spa_dstdir]))
doxyfile_conf.set('cssfiles', ' '.join(cssfiles))

doxyfile = configure_file(input: 'Doxyfile.in',
                          output: 'Doxyfile',
                          configuration: doxyfile_conf)

docdir = get_option('docdir')
if docdir == ''
  docdir = pipewire_datadir / 'doc' / meson.project_name()
endif

html_target = custom_target('pipewire-docs',
                            input: [ doxyfile ] + inputs + cssfiles,
                            output: [ 'html' ],
                            command: [ doxygen, doxyfile ],
                            depends: spa_strip_static,
                            install: true,
                            install_dir: docdir)
