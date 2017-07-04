%global majorminor   0.1

#global snap       20141103
#global gitrel     327
#global gitcommit  aec811798cd883a454b9b5cd82c77831906bbd2d
#global shortcommit %(c=%{gitcommit}; echo ${c:0:5})

# https://bugzilla.redhat.com/983606
%global _hardened_build 1

# where/how to apply multilib hacks
%global multilib_archs x86_64 %{ix86} ppc64 ppc s390x s390 sparc64 sparcv9 ppc64le

Name:           pipewire
Summary:        Media Sharing Server
Version:        0.1.1.1
Release:        1%{?snap:.%{snap}git%{shortcommit}}%{?dist}
License:        LGPLv2+
URL:            http://www.freedesktop.org/wiki/Software/PipeWire
%if 0%{?gitrel}
# git clone git://anongit.freedesktop.org/gstreamer/pipewire
# cd pipewire; git reset --hard %{gitcommit}; ./autogen.sh; make; make distcheck
Source0:        pipewire-%{version}-%{gitrel}-g%{shortcommit}.tar.gz
%else
Source0:        http://freedesktop.org/software/pipewire/releases/pipewire-%{version}.tar.gz
%endif

## upstream patches

## upstreamable patches

BuildRequires:  meson >= 0.35.0
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(glib-2.0) >= 2.32
BuildRequires:  pkgconfig(gio-unix-2.0) >= 2.32
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.10.0
BuildRequires:  pkgconfig(gstreamer-base-1.0) >= 1.10.0
BuildRequires:  pkgconfig(gstreamer-plugins-base-1.0) >= 1.10.0
BuildRequires:  pkgconfig(gstreamer-net-1.0) >= 1.10.0
BuildRequires:  pkgconfig(gstreamer-allocators-1.0) >= 1.10.0
BuildRequires:  systemd-devel >= 184
BuildRequires:  alsa-lib-devel
BuildRequires:  libv4l-devel
BuildRequires:  doxygen
BuildRequires:  xmltoman
BuildRequires:  graphviz

Requires(pre):  shadow-utils
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}
Requires:       systemd >= 184
Requires:       rtkit

%description
PipeWire is a multimedia server for Linux and other Unix like operating
systems.

%package libs
Summary:        Libraries for PipeWire clients
License:        LGPLv2+

%description libs
This package contains the runtime libraries for any application that wishes
to interface with a PipeWire media server.

%package devel
Summary:        Headers and libraries for PipeWire client development
License:        LGPLv2+
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}
%description devel
Headers and libraries for developing applications that can communicate with
a PipeWire media server.

%package utils
Summary:        PipeWire media server utilities
License:        LGPLv2+
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description utils
This package contains command line utilities for the PipeWire media server.

%prep
%setup -q -T -b0 -n %{name}-%{version}%{?gitrel:-%{gitrel}-g%{shortcommit}}

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test

%pre
getent group pipewire >/dev/null || groupadd -r pipewire
getent passwd pipewire >/dev/null || \
    useradd -r -g pipewire -d /var/run/pipewire -s /sbin/nologin -c "PipeWire System Daemon" pipewire
exit 0

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license LICENSE GPL LGPL
%doc README
%{_datadir}/doc/pipewire/html
%{_bindir}/pipewire
%{_libdir}/libpipewire-%{majorminor}.so.*
%{_libdir}/libpipewirecore-%{majorminor}.so.*
%{_libdir}/libspa-lib.so.*
%{_libdir}/gstreamer-1.0/libgstpipewire.*
%{_libdir}/pipewire-%{majorminor}/
%{_libdir}/spa/
%{_mandir}/man1/pipewire.1*
%{_sysconfdir}/pipewire/pipewire.conf

%files libs
%license LICENSE GPL LGPL
%doc README
%dir %{_sysconfdir}/pipewire/
#%dir %{_libdir}/pipewire/

%files devel
%{_libdir}/libpipewire-%{majorminor}.so
%{_libdir}/libpipewirecore-%{majorminor}.so
%{_libdir}/libspa-lib.so
%{_includedir}/pipewire/
%{_includedir}/spa/
%{_libdir}/pkgconfig/libpipewire-%{majorminor}.pc
%{_libdir}/pkgconfig/libpipewirecore-%{majorminor}.pc
%{_libdir}/pkgconfig/libspa-%{majorminor}.pc

%files utils
%{_bindir}/pipewire-monitor
%{_mandir}/man1/pipewire-monitor.1*
%{_bindir}/spa-monitor
%{_bindir}/spa-inspect

%changelog
* Mon Jun 26 2017 Wim Taymans <wtaymans@redhat.com> - 0.1.1-1
- Update to 0.1.1
- Add dbus-1 to BuildRequires
- change libs-devel to -devel

* Wed Sep 9 2015 Wim Taymans <wtaymans@redhat.com> - 0.1.0-2
- Fix BuildRequires to use pkgconfig, add all dependencies found in configure.ac
- Add user and groups  if needed
- Add license to %%licence

* Tue Sep 1 2015 Wim Taymans <wtaymans@redhat.com> - 0.1.0-1
- First version
