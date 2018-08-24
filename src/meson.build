pipewire_jack_sources = [
  'pipewire-jack.c',
]

pipewire_jack_c_args = [
  '-DHAVE_CONFIG_H',
  '-DPIC',
]

#optional dependencies
jack_dep = dependency('jack', version : '>= 1.9.10', required : false)

pipewire_jack = shared_library('jack-pipewire',
    pipewire_jack_sources,
    c_args : pipewire_jack_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep],
    install : false,
)
