#subdir('gst')
subdir('spa')

pinos_module_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
]

pinos_module_flatpak = shared_library('pinos-module-flatpak', [ 'module-flatpak.c' ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [dbus_dep, mathlib, dl_lib, pinos_dep, pinoscore_dep],
)

pinos_module_autolink = shared_library('pinos-module-autolink', [ 'module-autolink.c' ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [mathlib, dl_lib, pinos_dep, pinoscore_dep],
)

pinos_module_mixer = shared_library('pinos-module-mixer', [ 'module-mixer.c' ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [mathlib, dl_lib, pinos_dep, pinoscore_dep],
)

pinos_module_protocol_dbus = shared_library('pinos-module-protocol-dbus', [ 'module-protocol-dbus.c', gdbus_target ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [glib_dep, gio_dep, mathlib, dl_lib, pinos_dep, pinoscore_dep],
)

pinos_module_protocol_native = shared_library('pinos-module-protocol-native', [ 'module-protocol-native.c' ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [mathlib, dl_lib, pinos_dep, pinoscore_dep],
)

pinos_module_suspend_on_idle = shared_library('pinos-module-suspend-on-idle', [ 'module-suspend-on-idle.c' ],
  c_args : pinos_module_c_args,
  include_directories : [configinc, spa_inc],
  link_with : spalib,
  install : true,
  install_dir : '@0@/pinos-@1@'.format(get_option('libdir'), apiversion),
  dependencies : [mathlib, dl_lib, pinos_dep, pinoscore_dep],
)
