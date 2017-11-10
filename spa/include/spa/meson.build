spa_headers = [
  'buffer/buffer.h',
  'buffer/meta.h',
  'clock/clock.h',
  'graph/graph.h',
  'monitor/monitor.h',
  'node/command.h',
  'node/event.h',
  'node/node.h',
  'param/param.h',
  'param/props.h',
  'param/buffers.h',
  'param/meta.h',
  'param/format.h',
  'param/format-utils.h',
  'pod/pod.h',
  'pod/builder.h',
  'pod/command.h',
  'pod/event.h',
  'pod/iter.h',
  'pod/parser.h',
  'support/log.h',
  'support/log-impl.h',
  'support/loop.h',
  'support/plugin.h',
  'support/type-map.h',
  'support/type-map-impl.h',
  'utils/defs.h',
  'utils/dict.h',
  'utils/hook.h',
  'utils/list.h',
  'utils/ringbuffer.h',
  'utils/type.h',
]

install_headers(spa_headers, subdir : 'spa')

spa_audio_headers = [
  'param/audio/format.h',
  'param/audio/format-utils.h',
  'param/audio/raw.h',
  'param/audio/raw-utils.h',
]

install_headers(spa_audio_headers,
  subdir : 'spa/param/audio')

spa_video_headers = [
  'param/video/chroma.h',
  'param/video/color.h',
  'param/video/encoded.h',
  'param/video/format.h',
  'param/video/format-utils.h',
  'param/video/multiview.h',
  'param/video/raw.h',
  'param/video/raw-utils.h',
]

install_headers(spa_video_headers,
  subdir : 'spa/param/video')
