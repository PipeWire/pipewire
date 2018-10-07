FROM ubuntu:18.10
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update -qq && apt-get install -y gcc expect build-essential curl python3 pkg-config
RUN apt-get -y install doxygen python3-pip meson
RUN apt-get -y remove --auto-remove libdbus-1-3
RUN dpkg --remove libdbus-1-3-udeb
# RUN if [ `arch` = "x86_64" ]; then curl -L -O ; dpkg -i ; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pcre3/libpcre16-3_8.39-11_amd64.deb; dpkg -i libpcre16-3_8.39-11_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pcre3/libpcre32-3_8.39-11_amd64.deb; dpkg -i libpcre32-3_8.39-11_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pcre3/libpcrecpp0v5_8.39-11_amd64.deb; dpkg -i libpcrecpp0v5_8.39-11_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pcre3/libpcre3-dev_8.39-11_amd64.deb; dpkg -i libpcre3-dev_8.39-11_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/e/elfutils/libelf1_0.170-0.5_amd64.deb; dpkg -i libelf1_0.170-0.5_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/glib2.0/libglib2.0-dev-bin_2.58.1-2_amd64.deb; dpkg -i libglib2.0-dev-bin_2.58.1-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/glib2.0/libglib2.0-bin_2.58.1-2_amd64.deb; dpkg -i libglib2.0-bin_2.58.1-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/z/zlib/zlib1g-dev_1.2.11.dfsg-0ubuntu2_amd64.deb; dpkg -i zlib1g-dev_1.2.11.dfsg-0ubuntu2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/e/elfutils/libelf-dev_0.170-0.5_amd64.deb; dpkg -i libelf-dev_0.170-0.5_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/glib2.0/libglib2.0-dev_2.58.1-2_amd64.deb; dpkg -i libglib2.0-dev_2.58.1-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/d/dbus/libdbus-1-3_1.12.10-1ubuntu2_amd64.deb; dpkg -i libdbus-1-3_1.12.10-1ubuntu2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libs/libsamplerate/libsamplerate0_0.1.9-2_amd64.deb; dpkg -i libsamplerate0_0.1.9-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libs/libsamplerate/libsamplerate0-dev_0.1.9-2_amd64.deb; dpkg -i libsamplerate0-dev_0.1.9-2_amd64.deb ; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/j/jackd2/libjack-jackd2-0_1.9.12~dfsg-2_amd64.deb; dpkg -i libjack-jackd2-0_1.9.12~dfsg-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/d/dbus/libdbus-1-dev_1.12.10-1ubuntu2_amd64.deb; dpkg -i libdbus-1-dev_1.12.10-1ubuntu2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/j/jackd2/libjack-jackd2-dev_1.9.12~dfsg-2_amd64.deb; dpkg -i libjack-jackd2-dev_1.9.12~dfsg-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/t/tcp-wrappers/libwrap0_7.6.q-27_amd64.deb; dpkg -i libwrap0_7.6.q-27_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/t/tcp-wrappers/libwrap0-dev_7.6.q-27_amd64.deb; dpkg -i libwrap0-dev_7.6.q-27_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/glibc/multiarch-support_2.28-0ubuntu1_amd64.deb; dpkg -i multiarch-support_2.28-0ubuntu1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libo/libogg/libogg0_1.3.2-1_amd64.deb; dpkg -i libogg0_1.3.2-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libv/libvorbis/libvorbis0a_1.3.6-1_amd64.deb; dpkg -i libvorbis0a_1.3.6-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libv/libvorbis/libvorbisenc2_1.3.6-1_amd64.deb; dpkg -i libvorbisenc2_1.3.6-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/f/flac/libflac8_1.3.2-3_amd64.deb; dpkg -i libflac8_1.3.2-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/t/tcp-wrappers/libwrap0-dev_7.6.q-27_amd64.deb; dpkg -i libwrap0-dev_7.6.q-27_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libs/libsndfile/libsndfile1_1.0.28-4_amd64.deb; dpkg -i libsndfile1_1.0.28-4_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libo/libogg/libogg-dev_1.3.2-1_amd64.deb; dpkg -i libogg-dev_1.3.2-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libv/libvorbis/libvorbisfile3_1.3.6-1_amd64.deb; dpkg -i libvorbisfile3_1.3.6-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libv/libvorbis/libvorbis-dev_1.3.6-1_amd64.deb; dpkg -i libvorbis-dev_1.3.6-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/f/flac/libflac-dev_1.3.2-3_amd64.deb; dpkg -i libflac-dev_1.3.2-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libs/libsndfile/libsndfile1-dev_1.0.28-4_amd64.deb; dpkg -i libsndfile1-dev_1.0.28-4_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/liba/libasyncns/libasyncns0_0.8-6_amd64.deb; dpkg -i libasyncns0_0.8-6_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxau/libxau6_1.0.8-1_amd64.deb; dpkg -i libxau6_1.0.8-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libb/libbsd/libbsd0_0.9.1-1_amd64.deb; dpkg -i libbsd0_0.9.1-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxdmcp/libxdmcp6_1.1.2-3_amd64.deb; dpkg -i libxdmcp6_1.1.2-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxcb/libxcb1_1.13-3_amd64.deb; dpkg -i libxcb1_1.13-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libp/libpthread-stubs/libpthread-stubs0-dev_0.3-4_amd64.deb; dpkg -i libpthread-stubs0-dev_0.3-4_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/x/xorg-sgml-doctools/xorg-sgml-doctools_1.11-1_all.deb; dpkg -i xorg-sgml-doctools_1.11-1_all.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/x/xorgproto/x11proto-dev_2018.4-4_all.deb; dpkg -i x11proto-dev_2018.4-4_all.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/x/xorgproto/x11proto-core-dev_2018.4-4_all.deb; dpkg -i x11proto-core-dev_2018.4-4_all.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxau/libxau-dev_1.0.8-1_amd64.deb; dpkg -i libxau-dev_1.0.8-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxdmcp/libxdmcp-dev_1.1.2-3_amd64.deb; dpkg -i libxdmcp-dev_1.1.2-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libx/libxcb/libxcb1-dev_1.13-3_amd64.deb; dpkg -i libxcb1-dev_1.13-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/a/apparmor/libapparmor1_2.12-4ubuntu8_amd64.deb; dpkg -i libapparmor1_2.12-4ubuntu8_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libasound2-data; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/a/alsa-lib/libasound2_1.1.6-1ubuntu1_amd64.deb; dpkg -i libasound2_1.1.6-1ubuntu1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libc/libcap2/libcap2_2.25-1.2_amd64.deb; dpkg -i libcap2_2.25-1.2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install x11-common && curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libi/libice/libice6_1.0.9-2_amd64.deb; dpkg -i libice6_1.0.9-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libt/libtool/libltdl7_2.4.6-4_amd64.deb; dpkg -i libltdl7_2.4.6-4_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/o/orc/liborc-0.4-0_0.4.28-2_amd64.deb; dpkg -i liborc-0.4-0_0.4.28-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libltdl-dev && curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libt/libtool/libltdl-dev_2.4.6-4_amd64.deb; dpkg -i libltdl-dev_2.4.6-4_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libc/libcap2/libcap-dev_2.25-1.2_amd64.deb; dpkg -i libcap-dev_2.25-1.2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/libs/libsm/libsm6_1.2.2-1_amd64.deb; dpkg -i libsm6_1.2.2-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libspeex-dev libspeexdsp1; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/t/tdb/libtdb1_1.3.16-1_amd64.deb; dpkg -i libtdb1_1.3.16-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libwebrtc-audio-processing1 libx11-6 libx11-dev libx11-xcb1 libx11-xcb-dev libxtst6 libasound2-plugins pulseaudio-utils; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pulseaudio/libpulse0_11.1-1ubuntu7.1_amd64.deb; dpkg -i libpulse0_11.1-1ubuntu7.1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://launchpadlibrarian.net/371711450/libpulse-mainloop-glib0_11.1-1ubuntu7.1_amd64.deb; dpkg -i libpulse-mainloop-glib0_11.1-1ubuntu7.1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pulseaudio/libpulse0_11.1-1ubuntu7.1_amd64.deb; dpkg -i libpulse0_11.1-1ubuntu7.1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://launchpadlibrarian.net/371711462/pulseaudio_11.1-1ubuntu7.1_amd64.deb; dpkg -i pulseaudio_11.1-1ubuntu7.1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt -y --fix-broken install; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libasound2-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libglu1-mesa-dev libibus-1.0-dev  libmirclient-dev libpulse-dev libsdl2-2.0-0 libsndio-dev libudev-dev libwayland-dev libxcursor-dev libxext-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev libxss-dev libxt-dev libxv-dev libxxf86vm-dev libsdl2-dev; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libavcodec58 libavutil-dev libswresample-dev && curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavcodec-dev_4.0.2-2_amd64.deb; dpkg -i libavcodec-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libavformat58 && curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavformat-dev_4.0.2-2_amd64.deb; dpkg -i libavformat-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libavfilter7 libpostproc-dev libswscale-dev && curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavfilter-dev_4.0.2-2_amd64.deb; dpkg -i libavfilter-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libva-glx2 libva-wayland2 libset-scalar-perl && curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/libv/libva/libva-dev_2.2.0-1_amd64.deb; dpkg -i libva-dev_2.2.0-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libsbc1 && curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/sbc/libsbc-dev_1.3-3_amd64.deb; dpkg -i libsbc-dev_1.3-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/systemd/libudev-dev_239-7ubuntu9_amd64.deb; dpkg -i libudev-dev_239-7ubuntu9_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/speex/libspeexdsp-dev_1.2~rc1.2-1ubuntu2_amd64.deb; dpkg -i libspeexdsp-dev_1.2~rc1.2-1ubuntu2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then apt-get -y install libgstreamer1.0-0 && curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gstreamer1.0/gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; dpkg -i gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gobject-introspection/gir1.2-glib-2.0_1.58.0-1_amd64.deb; dpkg -i gir1.2-glib-2.0_1.58.0-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gstreamer1.0/gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; dpkg -i gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/d/doxygen/doxygen_1.8.13-10ubuntu1_amd64.deb; dpkg -i doxygen_1.8.13-10ubuntu1_amd64.deb; fi
COPY meson.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/meson.sh
RUN mkdir /build
WORKDIR /build
ENTRYPOINT ["meson.sh"]
