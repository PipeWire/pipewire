spa_vulkan_sources = ['plugin.c',
                'vulkan-compute-source.c',
                'vulkan-utils.c']

spa_vulkan = shared_library('spa-vulkan',
                           spa_vulkan_sources,
                           include_directories : [spa_inc],
                           dependencies : [ vulkan_dep, mathlib ],
                           install : true,
                           install_dir : '@0@/spa/vulkan'.format(get_option('libdir')))
