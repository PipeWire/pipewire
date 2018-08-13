spalib_headers = [
  'debug.h',
]

install_headers(spalib_headers, subdir : 'spa/lib')

spalib_sources = ['debug.c' ]

spalib = shared_library('spa-lib-@0@'.format(spaversion),
                         spalib_sources,
                         version : libversion,
                         soversion : soversion,
                         include_directories : [ spa_inc, spa_libinc ],
                         install : true)

spalib_dep = declare_dependency(link_with : spalib,
  include_directories : spa_inc,
)
