
subdir('pipewire')
subdir('daemon')
subdir('tools')
subdir('modules')
subdir('examples')
if get_option('tests').allowed()
  subdir('tests')
endif

if gst_dep.length() != 0
  subdir('gst')
endif
