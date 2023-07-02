spa_vulkan_sources = [
  'plugin.c',
  'vulkan-compute-filter.c',
  'vulkan-compute-source.c',
  'vulkan-compute-utils.c',
  'vulkan-utils.c'
]

drm = dependency('libdrm')

if cc.has_header('linux/dma-buf.h') and target_machine.system() == 'linux'
  spa_vulkan_sources += files('dmabuf_linux.c')
else
  spa_vulkan_sources += files('dmabuf_fallback.c')
endif

spa_vulkan = shared_library('spa-vulkan',
  spa_vulkan_sources,
  dependencies : [ spa_dep, vulkan_dep, mathlib, drm ],
  install : true,
  install_dir : spa_plugindir / 'vulkan')
