pipewire_alsa_plugin_sources = [
  'pcm_pipewire.c',
]

pipewire_alsa_plugin_c_args = [
  '-DHAVE_CONFIG_H',
  '-DPIC',
]

pipewire_alsa_plugin = shared_library('asound_module_pcm_pipewire',
    pipewire_alsa_plugin_sources,
    c_args : pipewire_alsa_plugin_c_args,
    include_directories : [configinc],
    dependencies : [pipewire_dep, alsa_dep],
    install : true,
    install_dir : join_paths(pipewire_libdir, 'alsa-lib'),
)
