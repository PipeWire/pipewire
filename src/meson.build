
subdir('pipewire')
subdir('daemon')
subdir('tools')
subdir('modules')
if get_option('examples').allowed()
  subdir('examples')
endif
if get_option('tests').allowed()
  subdir('tests')
endif

if gst_dep.length() != 0
  subdir('gst')
endif
