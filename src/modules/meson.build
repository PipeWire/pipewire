subdir('spa')

pipewire_module_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]

if dbus_dep.found()
pipewire_module_flatpak = shared_library('pipewire-module-flatpak', [ 'module-flatpak.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_rtkit = shared_library('pipewire-module-rtkit', [ 'module-rtkit.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep],
)
endif

pipewire_module_mixer = shared_library('pipewire-module-mixer',
  [ 'module-mixer.c', 'spa/spa-node.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_client_node = shared_library('pipewire-module-client-node',
  [ 'module-client-node.c',
    'module-client-node/client-node.c',
    'module-client-node/client-stream.c',
    'module-client-node/protocol-native.c',
    'spa/spa-node.c', ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_link_factory = shared_library('pipewire-module-link-factory',
  [ 'module-link-factory.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

#pipewire_module_protocol_dbus = shared_library('pipewire-module-protocol-dbus', [ 'module-protocol-dbus.c', gdbus_target ],
#  c_args : pipewire_module_c_args,
#  include_directories : [configinc, spa_inc],
#  link_with : spalib,
#  install : true,
#  install_dir : modules_install_dir,
#  dependencies : [glib_dep, gio_dep, mathlib, dl_lib, pipewire_dep],
#)

pipewire_module_protocol_native = shared_library('pipewire-module-protocol-native',
  [ 'module-protocol-native.c',
    'module-protocol-native/local-socket.c',
    'module-protocol-native/portal-screencast.c',
    'module-protocol-native/protocol-native.c',
    'module-protocol-native/connection.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)

pipewire_module_audio_session = shared_library('pipewire-module-media-session',
  [ 'module-media-session.c',
    'module-media-session/audio-dsp.c',
    'spa/spa-node.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, rt_lib, pipewire_dep],
)

pipewire_module_suspend_on_idle = shared_library('pipewire-module-suspend-on-idle', [ 'module-suspend-on-idle.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep],
)
