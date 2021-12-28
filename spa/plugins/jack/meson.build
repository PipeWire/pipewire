spa_jack_sources = [
'plugin.c',
                'jack-client.c',
                'jack-device.c',
                'jack-sink.c',
                'jack-source.c']

spa_jack = shared_library('spa-jack',
  spa_jack_sources,
  dependencies : [ spa_dep, jack_dep, mathlib ],
  install : true,
  install_dir : spa_plugindir / 'jack')
