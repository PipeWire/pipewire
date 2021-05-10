conf_files = [
  [ 'demonic.conf', 'demonic.conf' ],
  [ 'sink-eq6.conf', 'sink-eq6.conf' ],
  [ 'source-rnnoise.conf', 'source-rnnoise.conf' ],
]

foreach c : conf_files
  configure_file(input : c.get(0),
                 output : c.get(1),
                 configuration : conf_config,
                 install_dir : conf_install_dir / 'filter-chain')
endforeach
