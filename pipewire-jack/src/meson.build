pipewire_jack_sources = [
  'pipewire-jack.c',
  'ringbuffer.c',
  'uuid.c',
]

pipewire_jack_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DPIC',
]

#optional dependencies
jack_dep = dependency('jack', version : '>= 1.9.10', required : false)

pipewire_jack = shared_library('jack-pw',
    pipewire_jack_sources,
    soversion : pipewire_version,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep, mathlib],
    install : true,
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
