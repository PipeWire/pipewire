executable('spa-inspect', 'spa-inspect.c',
           dependencies : [ spa_dep, dl_lib ],
           install : true)

executable('spa-monitor', 'spa-monitor.c',
           dependencies : [ spa_dep, dl_lib ],
           install : true)

spa_json_dump = executable('spa-json-dump', 'spa-json-dump.c',
           dependencies : [ spa_dep ],
           install : true)

spa_json_dump_exe = executable('spa-json-dump-native', 'spa-json-dump.c',
           dependencies : [ spa_inc_dep ],
           native : true)

meson.override_find_program('spa-json-dump', spa_json_dump_exe)
