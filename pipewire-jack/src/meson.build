pipewire_jack_sources = [
  'pipewire-jack.c',
  'ringbuffer.c',
  'uuid.c',
]

pipewire_dummy_sources = [
  'dummy.c',
]

pipewire_jack_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DPIC',
]

#optional dependencies
jack_dep = dependency('jack', version : '>= 1.9.10', required : false)

libjack_path = get_option('libjack-path')
if libjack_path == ''
  libjack_path = join_paths(modules_install_dir, 'jack')
  libjack_path_dlopen = join_paths(modules_install_dir_dlopen, 'jack')
else
  libjack_path_dlopen = libjack_path
endif

tools_config = configuration_data()
tools_config.set('LIBJACK_PATH', libjack_path_dlopen)

configure_file(input : 'pw-jack.in',
               output : 'pw-jack',
               configuration : tools_config,
               install_dir : pipewire_bindir)

pipewire_jack = shared_library('jack',
    pipewire_jack_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)

pipewire_jackserver = shared_library('jackserver',
    pipewire_dummy_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)

pipewire_jackserver = shared_library('jacknet',
    pipewire_dummy_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep, mathlib],
    install : true,
    install_dir : libjack_path,
)

if sdl_dep.found()
  executable('video-dsp-play',
    '../examples/video-dsp-play.c',
    c_args : [ '-D_GNU_SOURCE' ],
    install: false,
    dependencies : [jack_dep, sdl_dep, mathlib],
    link_with: pipewire_jack,
  )
endif
