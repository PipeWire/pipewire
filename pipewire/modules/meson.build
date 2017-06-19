subdir('spa')

pipewire_module_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]

pipewire_module_flatpak = shared_library('pipewire-module-flatpak', [ 'module-flatpak.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [dbus_dep, mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)

pipewire_module_autolink = shared_library('pipewire-module-autolink', [ 'module-autolink.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)

pipewire_module_mixer = shared_library('pipewire-module-mixer', [ 'module-mixer.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)

pipewire_module_client_node = shared_library('pipewire-module-client-node', [ 'module-client-node.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)

#pipewire_module_protocol_dbus = shared_library('pipewire-module-protocol-dbus', [ 'module-protocol-dbus.c', gdbus_target ],
#  c_args : pipewire_module_c_args,
#  include_directories : [configinc, spa_inc],
#  link_with : spalib,
#  install : true,
#  install_dir : modules_install_dir,
#  dependencies : [glib_dep, gio_dep, mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
#)

pipewire_module_protocol_native = shared_library('pipewire-module-protocol-native', [ 'module-protocol-native.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)

pipewire_module_suspend_on_idle = shared_library('pipewire-module-suspend-on-idle', [ 'module-suspend-on-idle.c' ],
  c_args : pipewire_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : modules_install_dir,
  dependencies : [mathlib, dl_lib, pipewire_dep, pipewirecore_dep],
)
