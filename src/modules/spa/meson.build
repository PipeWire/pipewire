pipewire_module_spa_node = shared_library('pipewire-module-spa-node',
  [ 'module-node.c', 'spa-node.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_spa_device = shared_library('pipewire-module-spa-device',
  [ 'module-device.c', 'spa-device.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_spa_node_factory = shared_library('pipewire-module-spa-node-factory',
  [ 'module-node-factory.c', 'spa-node.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)

pipewire_module_spa_device_factory = shared_library('pipewire-module-spa-device-factory',
  [ 'module-device-factory.c', 'spa-device.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : modules_install_dir,
  dependencies : [spa_dep, mathlib, dl_lib, pipewire_dep],
)
