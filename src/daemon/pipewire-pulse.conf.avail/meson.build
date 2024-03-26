conf_files = [
 '20-upmix.conf',
]

foreach c : conf_files
  res = configure_file(input : '@0@.in'.format(c),
                 output : c,
                 configuration : conf_config,
                 install_dir : pipewire_confdatadir / 'pipewire-pulse.conf.avail')
  test(f'validate-json-pulse-@c@', spa_json_dump_exe, args : res)
endforeach

