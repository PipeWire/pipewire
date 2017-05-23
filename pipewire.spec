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
Version:        0.1
Release:        2%{?snap:.%{snap}git%{shortcommit}}%{?dist}
License:        LGPLv2+
URL:            http://www.freedesktop.org/wiki/Software/PipeWire
%if 0%{?gitrel}
# git clone git://anongit.freedesktop.org/gstreamer/pipewire
# cd pipewire; git reset --hard %{gitcommit}; ./autogen.sh; make; make distcheck
Source0:        pipewire-%{version}-%{gitrel}-g%{shortcommit}.tar.xz
%else
Source0:        http://freedesktop.org/software/pipewire/releases/pipewire-%{version}.tar.xz
%endif

## upstream patches

## upstreamable patches

BuildRequires:  automake libtool
BuildRequires:  m4
BuildRequires:  libtool-ltdl-devel
BuildRequires:  intltool
BuildRequires:  pkgconfig
BuildRequires:  xmltoman
BuildRequires:  tcp_wrappers-devel
BuildRequires:  pkgconfig(glib-2.0) >= 2.32
BuildRequires:  pkgconfig(gio-unix-2.0) >= 2.32
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.5.0
BuildRequires:  pkgconfig(gstreamer-base-1.0) >= 1.5.0
BuildRequires:  pkgconfig(gstreamer-plugins-base-1.0) >= 1.5.0
BuildRequires:  pkgconfig(gstreamer-net-1.0) >= 1.5.0
BuildRequires:  pkgconfig(gstreamer-allocators-1.0) >= 1.5.0
BuildRequires:  systemd-devel >= 184

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

%package libs-devel
Summary:        Headers and libraries for PipeWire client development
License:        LGPLv2+
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}
%description libs-devel
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

%if 0%{?gitrel:1}
# fixup PACKAGE_VERSION that leaks into pkgconfig files and friends
sed -i.PACKAGE_VERSION -e "s|^PACKAGE_VERSION=.*|PACKAGE_VERSION=\'%{version}\'|" configure
%else
## kill rpaths
%if "%{_libdir}" != "/usr/lib"
sed -i -e 's|"/lib /usr/lib|"/%{_lib} %{_libdir}|' configure
%endif
%endif


%build
%configure \
  --disable-silent-rules \
  --disable-static \
  --disable-rpath \
  --with-system-user=pipewire \
  --with-system-group=pipewire \
  --with-access-group=pipewire-access \
  --disable-systemd-daemon \
  --enable-tests

make %{?_smp_mflags} V=1

%install
make install DESTDIR=$RPM_BUILD_ROOT

## unpackaged files
# extraneous libtool crud
rm -fv $RPM_BUILD_ROOT%{_libdir}/*.la

%check
# don't fail build due failing tests on big endian arches (rhbz#1067470)
make check \
%ifarch ppc %{power64} s390 s390x
  || :
%else
  %{nil}
%endif


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
%{_sysconfdir}/xdg/autostart/pipewire.desktop
## already owned by -libs, see also https://bugzilla.redhat.com/show_bug.cgi?id=909690
#dir %{_sysconfdir}/pipewire/
%{_sysconfdir}/dbus-1/system.d/pipewire-system.conf
%{_bindir}/pipewire
%{_libdir}/libpipewire-%{majorminor}.so
%{_libdir}/libpipewirecore-%{majorminor}.so
%{_libdir}/gstreamer-1.0/libgstpipewire.*
%{_mandir}/man1/pipewire.1*

%files libs
%license LICENSE GPL LGPL
%doc README
#%dir %{_sysconfdir}/pipewire/
#%dir %{_libdir}/pipewire/

%files libs-devel
%{_includedir}/pipewire/
%{_libdir}/pkgconfig/libpipewire*.pc

%files utils
%{_bindir}/pipewire-monitor
%{_mandir}/man1/pipewire-monitor.1*

%changelog
* Wed Sep 9 2015 Wim Taymans <wtaymans@redhat.com> - 0.1.0-2
- Fix BuildRequires to use pkgconfig, add all dependencies found in configure.ac
- Add user and groups  if needed
- Add license to %%licence

* Tue Sep 1 2015 Wim Taymans <wtaymans@redhat.com> - 0.1.0-1
- First version
