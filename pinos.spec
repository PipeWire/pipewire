%global majorminor   0.1

#global snap       20141103
#global gitrel     327
#global gitcommit  aec811798cd883a454b9b5cd82c77831906bbd2d
#global shortcommit %(c=%{gitcommit}; echo ${c:0:5})

# https://bugzilla.redhat.com/983606
%global _hardened_build 1

# where/how to apply multilib hacks
%global multilib_archs x86_64 %{ix86} ppc64 ppc s390x s390 sparc64 sparcv9 ppc64le

Name:           pinos
Summary:        Media Sharing Server
Version:        0.1
Release:        1%{?snap:.%{snap}git%{shortcommit}}%{?dist}
License:        LGPLv2+
URL:            http://www.freedesktop.org/wiki/Software/Pinos
%if 0%{?gitrel}
# git clone git://anongit.freedesktop.org/gstreamer/pinos
# cd pinos; git reset --hard %{gitcommit}; ./autogen.sh; make; make distcheck
Source0:        pinos-%{version}-%{gitrel}-g%{shortcommit}.tar.xz
%else
Source0:        http://freedesktop.org/software/pinos/releases/pinos-%{version}.tar.xz
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
BuildRequires:  gstreamer1-devel >= 1.4.5
BuildRequires:  systemd-devel >= 184

Requires(pre):  shadow-utils
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}
Requires:       systemd >= 184
Requires:       rtkit

%description
Pinos is a multimedia server for Linux and other Unix like operating
systems.

%package libs
Summary:        Libraries for Pinos clients
License:        LGPLv2+

%description libs
This package contains the runtime libraries for any application that wishes
to interface with a Pinos media server.

%package libs-devel
Summary:        Headers and libraries for Pinos client development
License:        LGPLv2+
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}
%description libs-devel
Headers and libraries for developing applications that can communicate with
a Pinos media server.

%package utils
Summary:        Pinos media server utilities
License:        LGPLv2+
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description utils
This package contains command line utilities for the Pinos media server.

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
  --with-system-user=pinos \
  --with-system-group=pinos \
  --with-access-group=pinos-access \
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

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%doc README LICENSE GPL LGPL
%{_sysconfdir}/xdg/autostart/pinos.desktop
## already owned by -libs, see also https://bugzilla.redhat.com/show_bug.cgi?id=909690
#dir %{_sysconfdir}/pinos/
%{_sysconfdir}/dbus-1/system.d/pinos-system.conf
%{_bindir}/pinos
%{_libdir}/libpinos-%{majorminor}.so
%{_libdir}/libpinoscore-%{majorminor}.so
%{_libdir}/gstreamer-1.0/libgstpinos.*
%{_mandir}/man1/pinos.1*

%files libs 
%doc README LICENSE GPL LGPL
#%dir %{_sysconfdir}/pinos/
#%dir %{_libdir}/pinos/

%files libs-devel
%{_includedir}/pinos/
%{_libdir}/pkgconfig/libpinos*.pc

%files utils
%{_bindir}/pinos-monitor
%{_mandir}/man1/pinos-monitor.1*

%changelog
* Tue Sep 1 2015 Wim Taymans <wtaymans@redhat.com> - 0.1.0-1
- First version
