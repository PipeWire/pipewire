# pipewire-pulseaudio
PulseAudio client library for PipeWire

This is a replacement libpulse.so library. Clients using this library will
transparently connect to PipeWire.

This is now deprecated in favour of the protocol-pulse module that
implements the pulseaudio protocol directly. This makes it possible to
use the standard pulseaudio client library.
