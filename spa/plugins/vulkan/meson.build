spa_vulkan_sources = [
  'plugin.c',
  'vulkan-compute-filter.c',
  'vulkan-compute-source.c',
  'vulkan-utils.c'
]

spa_vulkan = shared_library('spa-vulkan',
  spa_vulkan_sources,
  dependencies : [ spa_dep, vulkan_dep, mathlib ],
  install : true,
  install_dir : spa_plugindir / 'vulkan')
