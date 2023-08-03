pipewire_jack_sources = [
  'export.c',
  'pipewire-jack.c',
  'ringbuffer.c',
  'uuid.c',
]

pipewire_jackserver_sources = pipewire_jack_sources
pipewire_jackserver_sources += [
  'control.c',
]

pipewire_net_sources = [
  'net.c',
]
pipewire_jack_c_args = [
  '-DPIC',
]

libjack_path = get_option('libjack-path')
if libjack_path == ''
  libjack_path = modules_install_dir / 'jack'
  libjack_path_dlopen = modules_install_dir_dlopen / 'jack'
  libjack_path_enable = ''
elif libjack_path == get_option('libdir') or libjack_path == pipewire_libdir
  libjack_path = pipewire_libdir
  libjack_path_dlopen = libjack_path
  libjack_path_enable = '#'
else
  libjack_path_dlopen = libjack_path
  libjack_path_enable = ''
endif

tools_config = configuration_data()
tools_config.set('LIBJACK_PATH', libjack_path_dlopen)
tools_config.set('LIBJACK_PATH_ENABLE', libjack_path_enable)

configure_file(input : 'pw-jack.in',
               output : 'pw-jack',
               configuration : tools_config,
               install_dir : pipewire_bindir)

pipewire_jack = shared_library('jack',
    pipewire_jack_sources,
    soversion : soversion,
    version : libjackversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc, jack_inc],
    dependencies : [pipewire_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)

pipewire_jackserver = shared_library('jackserver',
    pipewire_jackserver_sources,
    soversion : soversion,
    version : libjackversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc, jack_inc],
    dependencies : [pipewire_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)

pipewire_jacknet = shared_library('jacknet',
    pipewire_net_sources,
    soversion : soversion,
    version : libjackversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc, jack_inc],
    dependencies : [pipewire_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)


if get_option('jack-devel') == true
  if meson.version().version_compare('<0.59.0')
    error(
          '''
          Before version 0.59.0 Meson creates a wrong jack pkg-config file.
          For that reason this is now an error. Please update Meson,
          if you want to have JACK development files.
          ''')
  endif

  pkgconfig.generate(filebase : 'jack',
  libraries : [pipewire_jack],
  name : 'jack',
  description : 'PipeWire JACK API',
  version : '1.9.17',
  extra_cflags : '-D_REENTRANT',
  unescaped_variables: ['server_libs=-L${libdir} -ljackserver', 'jack_implementation=pipewire'])
endif

if sdl_dep.found()
  executable('video-dsp-play',
    '../examples/video-dsp-play.c',
    include_directories : [jack_inc],
    install : installed_tests_enabled,
    install_dir : installed_tests_execdir / 'examples' / 'jack',
    dependencies : [sdl_dep, mathlib],
    link_with: pipewire_jack,
  )
endif
