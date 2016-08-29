AC_PREREQ(2.63)

AC_INIT(pinos, 0.1)
AC_CONFIG_SRCDIR([pinos/daemon/main.c])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign 1.11 -Wall -Wno-portability subdir-objects silent-rules color-tests dist-xz tar-ustar])

AS_IF([! test -n "$VERSION"], [
   AC_MSG_ERROR([git-version-gen failed])
])

m4_define(pinos_major, `echo $VERSION | cut -d. -f1 | cut -d- -f1`)
m4_define(pinos_minor, `echo $VERSION | cut -d. -f2 | cut -d- -f1`)

AC_SUBST(PINOS_MAJOR, pinos_major)
AC_SUBST(PINOS_MINOR, pinos_minor)
AC_SUBST(PINOS_MAJORMINOR, pinos_major.pinos_minor)

AC_SUBST(PINOS_API_VERSION, 0)
AC_SUBST(PINOS_PROTOCOL_VERSION, 0)

# The stable ABI for client applications, for the version info x:y:z
# always will hold y=z
AC_SUBST(LIBPINOS_VERSION_INFO, [0:0:0])

dnl sets host_* variables
AC_CANONICAL_HOST

AC_DEFINE_UNQUOTED([CANONICAL_HOST], "$host", [Canonical host string.])

AC_CHECK_PROG([STOW], [stow], [yes], [no])

AS_IF([test "x$STOW" = "xyes" && test -d /usr/local/stow], [
    AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
    ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
])

AM_SILENT_RULES([yes])

#### Checks for programs. ####

# mkdir -p

AC_PROG_MKDIR_P

# ln -s

AC_PROG_LN_S

# CC

AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O
# Only required if you want the WebRTC canceller -- no runtime dep on
# libstdc++ otherwise
AC_PROG_CXX
AC_PROG_GCC_TRADITIONAL
AC_USE_SYSTEM_EXTENSIONS

# M4

AC_CHECK_PROGS([M4], gm4 m4, no)
AS_IF([test "x$M4" = "xno"], AC_MSG_ERROR([m4 missing]))

# pkg-config

PKG_PROG_PKG_CONFIG

# gettext

if test "x$enable_nls" != "xno"; then
IT_PROG_INTLTOOL([0.35.0])

AM_GNU_GETTEXT_VERSION([0.18.1])
AM_GNU_GETTEXT([external])

GETTEXT_PACKAGE=pinos
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[Gettext package])

pinoslocaledir='${prefix}/${DATADIRNAME}/locale'
AX_DEFINE_DIR(PINOS_LOCALEDIR, pinoslocaledir, [Gettext locale dir])
else
# workaround till an intltool m4 bug is fixed upstream
# (https://bugs.launchpad.net/intltool/+bug/904647)
USE_NLS=no
AC_SUBST(USE_NLS)
fi

AC_ARG_WITH([module-dir],
  AS_HELP_STRING([--with-module-dir=PATH],
    [Directory where modules are located (default is ${libdir}/pinos/modules]),
  [moduledir=$withval],
  [moduledir="${libdir}/pinos-${PINOS_MAJORMINOR}/modules"])
AC_SUBST(moduledir)
AX_DEFINE_DIR(MODULEDIR, moduledir, [Directory where modules are located])

#### Compiler flags ####

AX_APPEND_COMPILE_FLAGS(
    [-Wall -W -Wextra -pipe -Wno-long-long -Wno-overlength-strings -Wunsafe-loop-optimizations -Wundef -Wformat=2 -Wlogical-op -Wsign-compare -Wformat-security -Wmissing-include-dirs -Wformat-nonliteral -Wold-style-definition -Wpointer-arith -Winit-self -Wdeclaration-after-statement -Wfloat-equal -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls -Wmissing-declarations -Wmissing-noreturn -Wshadow -Wendif-labels -Wcast-align -Wstrict-aliasing -Wwrite-strings -Wno-unused-parameter -ffast-math -fno-common -fdiagnostics-show-option -fdiagnostics-color=auto],
    [], [-pedantic -Werror])

AS_CASE([" $CFLAGS "], [*" -O0 "*], [], [
    # Don't append the flag if it already exists.
    # Only enable fastpath asserts when doing a debug build, e.g. from bootstrap.sh.
    AX_APPEND_FLAG([-DFASTPATH], [CPPFLAGS])

    # Cannot use AX_APPEND_FLAG here, as it assumes no space inside the added flags.
    # Cannot append flags with AX_APPEND_FLAG one by one, as this would destroy all fortifications
    # if CPPFLAGS already contain -D_FORTIFY_SOURCE=2.

    # Warnings to be aware of that appear with -D_FORTIFY_SOURCE=2 but without -U_FORTIFY_SOURCE:
    # On Fedora 20 with -O0: #warning _FORTIFY_SOURCE requires compiling with optimization (-O) [-Wcpp]
    # On Gentoo with -O2:    "_FORTIFY_SOURCE" redefined [enabled by default]
    AS_VAR_APPEND([CPPFLAGS],[" -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2"])
])

#### Linker flags ####

# Check whether the linker supports the -version-script option.
# The Make variable $(srcdir) needs to be in the LDFLAGS in that form,
# so that it is expanded the right way in every subdir.
AX_CHECK_LINK_FLAG(["-Wl,-version-script=${srcdir}/pinos/map-file"],
    [VERSIONING_LDFLAGS='-Wl,-version-script=$(abs_top_srcdir)/pinos/map-file'])
AC_SUBST([VERSIONING_LDFLAGS])

# Use immediate (now) bindings; avoids the funky re-call in itself.
# The -z now syntax is lifted from Sun's linker and works with GNU's too, other linkers might be added later.
AX_APPEND_LINK_FLAGS([-Wl,-z,now], [IMMEDIATE_LDFLAGS])
AC_SUBST([IMMEDIATE_LDFLAGS])

# On ELF systems we don't want the libraries to be unloaded since we don't clean them up properly,
# so we request the nodelete flag to be enabled.
# On other systems, we don't really know how to do that, but it's welcome if somebody can tell.
AX_APPEND_LINK_FLAGS([-Wl,-z,nodelete], [NODELETE_LDFLAGS])
AC_SUBST([NODELETE_LDFLAGS])

# Check for the proper way to build libraries that have no undefined symbols
case $host in
    # FreeBSD (et al.) does not complete linking for shared objects when pthreads
    # are requested, as different implementations are present.
    *-freebsd* | *-openbsd*) ;;
    *)
        for possible_flag in "-Wl,--no-undefined" "-Wl,-z,defs"; do
            AX_CHECK_LINK_FLAG([$possible_flag], [NOUNDEFINED_LDFLAGS="$possible_flag"; break])
        done
    ;;
esac
AC_SUBST([NOUNDEFINED_LDFLAGS])

#### libtool stuff ####

LT_PREREQ(2.4)
LT_INIT([dlopen win32-dll disable-static])

dnl As an extra safety device, check for lt_dladvise_init() which is
dnl only implemented in libtool 2.x, and refine as we go if we have
dnl refined requirements.
dnl
dnl Check the header files first since the system may have a
dnl libltdl.so for runtime, but no headers, and we want to bail out as
dnl soon as possible.
dnl
dnl We don't need any special variable for this though, since the user
dnl can give the proper place to find libltdl through the standard
dnl variables like LDFLAGS and CPPFLAGS.

AC_CHECK_HEADER([ltdl.h],
    [AC_CHECK_LIB([ltdl], [lt_dladvise_init], [LIBLTDL=-lltdl], [LIBLTDL=])],
    [LIBLTDL=])

AS_IF([test "x$LIBLTDL" = "x"],
    [AC_MSG_ERROR([Unable to find libltdl version 2. Makes sure you have libtool 2.4 or later installed.])])
AC_SUBST([LIBLTDL])

###################################
#      External libraries         #
###################################

#### [lib]iconv ####

AM_ICONV

#### Valgrind (optional) ####

AC_CHECK_HEADERS_ONCE([valgrind/memcheck.h])

#### GLib ####

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.32, dummy=yes,
                    AC_MSG_ERROR(GLib >= 2.32 is required))
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(GIOUNIX, gio-unix-2.0 >= 2.32, dummy=yes,
                    AC_MSG_ERROR(GIO-Unix >= 2.32 is required))
GLIB_CFLAGS="$GLIB_CFLAGS $GIOUNIX_CFLAGS"
GLIB_LIBS="$GLIB_LIBS $GIOUNIX_LIBS"

PKG_CHECK_MODULES(GMODULE, gmodule-no-export-2.0 >= 2.32, dummy=yes,
                    AC_MSG_ERROR(GModule >= 2.32 is required))
GLIB_CFLAGS="$GLIB_CFLAGS $GMODULE_CFLAGS"
GLIB_LIBS="$GLIB_LIBS $GMODULE_LIBS"

AC_MSG_CHECKING([for glib-mkenums script])
GLIB_MKENUMS=`$PKG_CONFIG --variable=glib_mkenums glib-2.0`
AS_IF([test "x$GLIB_MKENUMS" = "x"],
  [AC_MSG_ERROR([glib-mkenums not listed in glib-2.0 pkg-config file])],
  [AC_SUBST([GLIB_MKENUMS])
   AC_MSG_RESULT([$GLIB_MKENUMS])])

AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

#### GStreamer ####

PKG_CHECK_MODULES(GST, gstreamer-1.0 >= 1.5.0, dummy=yes,
                    AC_MSG_ERROR(GStreamer >= 1.5.0 is required))
AC_SUBST(GST_CFLAGS)
AC_SUBST(GST_LIBS)

PKG_CHECK_MODULES(GSTBASE, gstreamer-base-1.0 >= 1.5.0, dummy=yes,
                    AC_MSG_ERROR(GStreamer Base>= 1.5.0 is required))
GST_CFLAGS="$GST_CFLAGS $GSTBASE_CFLAGS"
GST_LIBS="$GST_LIBS $GSTBASE_LIBS"

PKG_CHECK_MODULES(GSTPBASE, gstreamer-plugins-base-1.0 >= 1.5.0, dummy=yes,
                    AC_MSG_ERROR(GStreamer Plugins Base>= 1.5.0 is required))
GST_CFLAGS="$GST_CFLAGS $GSTPBASE_CFLAGS"
GST_LIBS="$GST_LIBS $GSTPBASE_LIBS"

PKG_CHECK_MODULES(GSTNET, gstreamer-net-1.0 >= 1.5.0, dummy=yes,
                    AC_MSG_ERROR(GStreamer Net>= 1.5.0 is required))
GST_CFLAGS="$GST_CFLAGS $GSTNET_CFLAGS"
GST_LIBS="$GST_LIBS $GSTNET_LIBS"

PKG_CHECK_MODULES(GSTALLOC, gstreamer-allocators-1.0 >= 1.5.0, dummy=yes,
                    AC_MSG_ERROR(GStreamer Allocators>= 1.5.0 is required))
GST_CFLAGS="$GST_CFLAGS $GSTALLOC_CFLAGS"
GST_LIBS="$GST_LIBS $GSTALLOC_LIBS"

#### ALSA ####
PKG_CHECK_MODULES(ALSA, alsa >= 0.9.1, dummy=yes,
                    AC_MSG_ERROR(ALSA >= 0.9.1 is required))

AC_SUBST(ALSA_CFLAGS)
AC_SUBST(ALSA_LIBS)

#### Build and Install man pages ####

AC_ARG_ENABLE([manpages],
    AS_HELP_STRING([--disable-manpages],[Disable building and installation of man pages]))

AM_CONDITIONAL([BUILD_MANPAGES], [test "x$enable_manpages" != "xno"])

dnl *** output files ***

dnl keep this alphabetic per directory, please
AC_CONFIG_FILES([
Makefile
pinos/Makefile
man/Makefile
man/pinos.1.xml
man/pinos-monitor.1.xml
pkgconfig/libpinos-$PINOS_MAJORMINOR.pc:pkgconfig/libpinos.pc.in
po/Makefile.in
])
AC_OUTPUT

echo "

Configuration
	Version                    : ${VERSION}
	Source code location       : ${srcdir}
	Prefix                     : ${prefix}
	Moduledir                  : ${moduledir}
	Compiler                   : ${CC}

pinos configured. Type 'make' to build.
"
