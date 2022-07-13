conf_files = [
  [ 'demonic.conf', 'demonic.conf' ],
  [ 'source-duplicate-FL.conf', 'source-duplicate-FL.conf' ],
  [ 'sink-mix-FL-FR.conf', 'sink-mix-FL-FR.conf' ],
  [ 'sink-make-LFE.conf', 'sink-make-LFE.conf' ],
  [ 'sink-virtual-surround-5.1-kemar.conf', 'sink-virtual-surround-5.1-kemar.conf' ],
  [ 'sink-virtual-surround-7.1-hesuvi.conf', 'sink-virtual-surround-7.1-hesuvi.conf' ],
  [ 'sink-dolby-surround.conf', 'sink-dolby-surround.conf' ],
  [ 'sink-eq6.conf', 'sink-eq6.conf' ],
  [ 'sink-matrix-spatialiser.conf', 'sink-matrix-spatialiser.conf' ],
  [ 'source-rnnoise.conf', 'source-rnnoise.conf' ],
]

foreach c : conf_files
  configure_file(input : c.get(0),
                 output : c.get(1),
                 configuration : conf_config,
                 install_dir : pipewire_confdatadir / 'filter-chain')
endforeach
