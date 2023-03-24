conf_files = [
 '20-upmix.conf',
]

foreach c : conf_files
  configure_file(input : '@0@.in'.format(c),
                 output : c,
                 configuration : conf_config,
                 install_dir : pipewire_confdatadir / 'pipewire-pulse.conf.avail')
endforeach

