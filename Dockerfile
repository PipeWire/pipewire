FROM ubuntu:18.10
RUN apt-get update -qq && apt-get install -y gcc expect build-essential curl python3 pkg-config
RUN apt-get -y install doxygen python3-pip build-dep meson
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/d/dbus/libdbus-1-dev_1.12.2-1ubuntu1_amd64.deb; dpkg -i libdbus-1-dev_1.12.2-1ubuntu1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/j/jackd2/libjack-jackd2-dev_1.9.12~dfsg-2_amd64.deb; dpkg -i libjack-jackd2-dev_1.9.12~dfsg-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/p/pulseaudio/libpulse-dev_11.1-1ubuntu7_amd64.deb; dpkg -i libpulse-dev_11.1-1ubuntu7_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/libs/libsdl2/libsdl2-dev_2.0.8+dfsg1-4ubuntu1_amd64.deb; dpkg -i libsdl2-dev_2.0.8+dfsg1-4ubuntu1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavcodec-dev_4.0.2-2_amd64.deb; dpkg -i libavcodec-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavformat-dev_4.0.2-2_amd64.deb; dpkg -i libavformat-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/f/ffmpeg/libavfilter-dev_4.0.2-2_amd64.deb; dpkg -i libavfilter-dev_4.0.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/libv/libva/libva-dev_2.2.0-1_amd64.deb; dpkg -i libva-dev_2.2.0-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/sbc/libsbc-dev_1.3-3_amd64.deb; dpkg -i libsbc-dev_1.3-3_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/systemd/libudev-dev_239-7ubuntu9_amd64.deb; dpkg -i libudev-dev_239-7ubuntu9_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/s/speex/libspeexdsp-dev_1.2~rc1.2-1ubuntu2_amd64.deb; dpkg -i libspeexdsp-dev_1.2~rc1.2-1ubuntu2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gstreamer1.0/gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; dpkg -i gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gobject-introspection/gir1.2-glib-2.0_1.58.0-1_amd64.deb; dpkg -i gir1.2-glib-2.0_1.58.0-1_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/g/gstreamer1.0/gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; dpkg -i gir1.2-gstreamer-1.0_1.14.2-2_amd64.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/universe/x/xmltoman/xmltoman_0.5-1_all.deb; dpkg -i xmltoman_0.5-1_all.deb; fi
RUN if [ `arch` = "x86_64" ]; then curl -L -O http://mirrors.kernel.org/ubuntu/pool/main/d/doxygen/doxygen_1.8.13-10ubuntu1_amd64.deb; dpkg -i doxygen_1.8.13-10ubuntu1_amd64.deb; fi
ENTRYPOINT ./meson.sh
