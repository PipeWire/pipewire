# This file is part of PulseVideo.
#
# Copyright 2015 Wim Taymans <wim.taymans@gmail.com>
#
# PulseVideo is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseVideo is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseVideo; if not, see <http://www.gnu.org/licenses/>.

###################################
#       Extra directories         #
###################################

pulsevideoincludedir=$(includedir)/client
pulsevideoserverincludedir=$(includedir)/server
pulsevideolibexecdir=$(libexecdir)/client
dbuspolicydir=$(sysconfdir)/dbus-1/system.d

###################################
#     Compiler/linker flags       #
###################################

AM_CPPFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/modules \
	-I$(top_builddir)/src/modules \
	-DPV_SRCDIR=\"$(abs_srcdir)\" \
	-DPV_BUILDDIR=\"$(abs_builddir)\"
AM_CFLAGS = $(GLIB_CFLAGS) $(GST_CFLAGS)
AM_CXXFLAGS = $(AM_CFLAGS)
SERVER_CFLAGS = -D__INCLUDED_FROM_PULSE_VIDEO

AM_LIBADD = $(GLIB_LIBS) $(INTLLIBS) $(GST_LIBS)
AM_LDADD = $(GLIB_LIBS) $(GST_LIBS) $(INTLLIBS)
AM_LDFLAGS = $(NODELETE_LDFLAGS)

FOREIGN_CFLAGS = -w

###################################
#          Extra files            #
###################################

EXTRA_DIST = \
		src/version.h.in \
		daemon/pulsevideo-system.conf

dbuspolicy_DATA = \
		daemon/pulsevideo-system.conf

###################################
#          Includes               #
###################################

enumtypesincludes = client/pv-context.h \
		    client/pv-source.h \
		    client/pv-stream.h \
		    client/pv-subscribe.h

client/pv-enumtypes.h: $(enumtypesincludes)
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#ifndef __PV_ENUM_TYPES_H__\n#define __PV_ENUM_TYPES_H__\n\n#include <glib-object.h>\n\nG_BEGIN_DECLS\n" \
	--fprod "\n/* enumerations from \"@filename@\" */\n" \
	--vhead "GType @enum_name@_get_type (void);\n#define PV_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\n"         \
	--ftail "G_END_DECLS\n\n#endif /* __PV_ENUM_TYPES_H__ */" \
	$^ > client/pv-enumtypes.h

client/pv-enumtypes.c: $(enumtypesincludes) client/pv-enumtypes.h
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#include \"pv-enumtypes.h\"\n#include <client/pulsevideo.h>\n#define C_ENUM(v) ((gint) v)\n#define C_FLAGS(v) ((guint) v)\n " \
	--fprod "\n/* enumerations from \"@filename@\" */" \
	--vhead "GType\n@enum_name@_get_type (void)\n{\n  static gsize id = 0;\n  static const G@Type@Value values[] = {"     \
	--vprod "    { C_@TYPE@(@VALUENAME@), \"@VALUENAME@\", \"@valuenick@\" }," \
	--vtail "    { 0, NULL, NULL }\n  };\n\n  if (g_once_init_enter (&id)) {\n    GType tmp = g_@type@_register_static (\"@EnumName@\", values);\n    g_once_init_leave (&id, tmp);\n  }\n\n  return (GType) id;\n}" \
	$^ > client/pv-enumtypes.c


dbus/org-pulsevideo.c: dbus/org-pulsevideo.h
dbus/org-pulsevideo.h: dbus/org.pulsevideo.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN) \
	--interface-prefix org.pulsevideo. \
	--generate-c-code dbus/org-pulsevideo \
	--generate-docbook ../doc/org-pulsevideo \
	--c-namespace Pv dbus/org.pulsevideo.xml \
        --c-generate-object-manager


built_header_make = client/pv-enumtypes.h dbus/org-pulsevideo.h
built_source_make = client/pv-enumtypes.c dbus/org-pulsevideo.c

BUILT_SOURCES = $(built_header_make)            \
		$(built_source_make)

CLEANFILES = $(built_header_make) $(built_source_make)

###################################
#          Main daemon            #
###################################

bin_PROGRAMS = pulsevideo

pulsevideo_SOURCES = \
		daemon/main.c

pulsevideo_CFLAGS = $(AM_CFLAGS)
pulsevideo_LDADD = $(AM_LDADD) libpulsevideocore-@PV_MAJORMINOR@.la libpulsevideo-@PV_MAJORMINOR@.la $(LIBLTDL)
# This is needed because automake doesn't properly expand the foreach below
pulsevideo_DEPENDENCIES = libpulsevideocore-@PV_MAJORMINOR@.la libpulsevideo-@PV_MAJORMINOR@.la
pulsevideo_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(IMMEDIATE_LDFLAGS)

###################################
#         Test programs           #
###################################
noinst_LTLIBRARIES =

TESTS_default =

TESTS_norun = test-client \
	      test-subscribe \
	      test-v4l2

# These tests need a running pulsevideo daemon
TESTS_daemon = 

noinst_PROGRAMS = $(TESTS_default) $(TESTS_norun) $(TESTS_daemon)

test_client_SOURCES = tests/test-client.c
test_client_CFLAGS = $(AM_CFLAGS)
test_client_LDADD = $(AM_LDADD) libpulsevideo-@PV_MAJORMINOR@.la
test_client_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

test_subscribe_SOURCES = tests/test-subscribe.c
test_subscribe_CFLAGS = $(AM_CFLAGS)
test_subscribe_LDADD = $(AM_LDADD) libpulsevideo-@PV_MAJORMINOR@.la
test_subscribe_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

test_v4l2_SOURCES = tests/test-v4l2.c
test_v4l2_CFLAGS = $(AM_CFLAGS)
test_v4l2_LDADD = $(AM_LDADD) libpulsevideo-@PV_MAJORMINOR@.la libpulsevideocore-@PV_MAJORMINOR@.la
test_v4l2_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Client library          #
###################################

pulsevideogstsource = gst/gstfdpay.h gst/gstfdpay.c \
		      gst/gstfddepay.h gst/gstfddepay.c \
		      gst/gsttmpfileallocator.h gst/gsttmpfileallocator.c \
		      wire-protocol.h

pulsevideoinclude_HEADERS = \
		client/pulsevideo.h \
		client/pv-context.h \
		client/pv-enumtypes.h \
		client/pv-stream.h \
		client/pv-subscribe.h \
		client/pv-source.h \
		client/pv-source-output.h \
		dbus/org-pulsevideo.h

lib_LTLIBRARIES = \
		libpulsevideo-@PV_MAJORMINOR@.la

# Public interface
libpulsevideo_@PV_MAJORMINOR@_la_SOURCES = \
		client/pv-context.h client/pv-context.c \
		client/pv-enumtypes.h client/pv-enumtypes.c \
		client/pv-stream.h client/pv-stream.c \
		client/pulsevideo.c client/pulsevideo.h \
		client/pv-source-output.c client/pv-source-output.h \
		client/pv-source.c client/pv-source.h \
		client/pv-subscribe.c client/pv-subscribe.h \
		dbus/org-pulsevideo.c \
		$(pulsevideogstsource)


libpulsevideo_@PV_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(GST_CFLAGS)
libpulsevideo_@PV_MAJORMINOR@_la_LDFLAGS = $(AM_LDFLAGS) -avoid-version
libpulsevideo_@PV_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) $(LTLIBICONV) $(GST_LIBS)

###################################
#      Daemon core library        #
###################################

lib_LTLIBRARIES += libpulsevideocore-@PV_MAJORMINOR@.la

# Pure core stuff
libpulsevideocore_@PV_MAJORMINOR@_la_SOURCES = \
		server/pv-client.c server/pv-client.h \
		server/pv-daemon.c server/pv-daemon.h \
		modules/v4l2/pv-v4l2-source.c 

libpulsevideocore_@PV_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(SERVER_CFLAGS)
libpulsevideocore_@PV_MAJORMINOR@_la_LDFLAGS = $(AM_LDFLAGS) -avoid-version
libpulsevideocore_@PV_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) $(LIBLTDL) $(LTLIBICONV)

###################################
#        Some minor stuff         #
###################################

DISTCLEANFILES = 

install-exec-hook:
	rm -f $(DESTDIR)$(modlibexecdir)/*.la

uninstall-hook:
	rm -f $(DESTDIR)$(modlibexecdir)/*.so

# Automatically generate linker version script. We use the same one for all public .sos
update-map-file:
	( echo "PULSE_0 {" ; \
	  echo "global:" ; \
	  ctags -I PV_GCC_MALLOC,PV_GCC_ALLOC_SIZE2,PV_GCC_ALLOC_SIZE,PV_GCC_PURE,PV_GCC_CONST,PV_GCC_DEPRECATED,PV_GCC_PRINTF_ATTR -f - --c-kinds=p $(pulsevideoinclude_HEADERS) | awk '/^pa_/ { print $$1 ";" }' | sort ; \
	  echo "local:" ;  \
	  echo "*;" ; \
	  echo "};" ) > $(srcdir)/map-file

update-all: update-map-file

# Force installation order of libraries. libtool relinks on install time, in
# which case libpulsecommon has to be installed before others, but the padsp
# preload library has to be done after the normal libraries (e.g. libpulse)
# ...
# Unfortunately automake behaviour means that rules without commands also
# override build-in rules, so it's not trivial to add dependencies.
# See http://debbugs.gnu.org/cgi/bugreport.cgi?bug=7328 for the workaround
# ...
# Isn't libtool/autotools fun!

installlibLTLIBRARIES = install-libLTLIBRARIES
$(installlibLTLIBRARIES): install-pkglibLTLIBRARIES

installmodlibexecLTLIBRARIES = install-modlibexecLTLIBRARIES
$(installmodlibexecLTLIBRARIES): install-pkglibLTLIBRARIES

installpadsplibLTLIBRARIES = install-padsplibLTLIBRARIES
$(installpadsplibLTLIBRARIES): install-libLTLIBRARIES

.PHONY: update-all update-map-file coverage
