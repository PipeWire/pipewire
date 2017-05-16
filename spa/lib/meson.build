spalib_headers = [
  'debug.h',
  'mapper.h',
  'props.h',
]

install_headers(spalib_headers, subdir : 'spa/lib')

spalib_sources = ['debug.c',
                  'mapper.c',
                  'props.c',
                  'format.c']

spalib = shared_library('spa-lib',
                         spalib_sources,
                         version : libversion,
                         soversion : soversion,
                         include_directories : [ spa_inc, spa_libinc ],
                         install : true)

spalib_dep = declare_dependency(link_with : spalib,
  include_directories : spa_inc,
)
