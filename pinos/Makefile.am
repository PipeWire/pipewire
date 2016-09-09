# This file is part of Pinos.
#
# Copyright 2015 Wim Taymans <wim.taymans@gmail.com>
#
# Pinos is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# Pinos is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Pinos; if not, see <http://www.gnu.org/licenses/>.

###################################
#       Extra directories         #
###################################

pinosincludedir=$(includedir)/pinos/client
pinosserverincludedir=$(includedir)/pinos/server
pinoslibexecdir=$(libexecdir)/pinos/client
xdgautostartdir=$(sysconfdir)/xdg/autostart
dbuspolicydir=$(sysconfdir)/dbus-1/system.d

###################################
#     Compiler/linker flags       #
###################################

AM_CPPFLAGS = \
	-I$(top_srcdir)/ \
	-I$(top_srcdir)/spa/include/ \
	-I$(top_srcdir)/pinos/modules \
	-I$(top_builddir)/pinos/modules \
	-DPINOS_SRCDIR=\"$(abs_srcdir)\" \
	-DPINOS_BUILDDIR=\"$(abs_builddir)\"
AM_CFLAGS = $(GLIB_CFLAGS)
AM_CXXFLAGS = $(AM_CFLAGS)
SERVER_CFLAGS = -D__INCLUDED_FROM_PINOS

AM_LIBADD = $(GLIB_LIBS) $(INTLLIBS) $(GST_LIBS) -L$(top_srcdir)/spa/build/lib/ -lspa-lib
AM_LDADD = $(GLIB_LIBS) $(GST_LIBS) $(INTLLIBS) -L$(top_srcdir)/spa/build/lib/ -lspa-lib
AM_LDFLAGS = $(NODELETE_LDFLAGS) -Wl,-rpath=/home/wim/gst/head/pinos/spa/build/lib/

FOREIGN_CFLAGS = -w

MODULE_LDFLAGS = $(AM_LDFLAGS) $(AM_LIBLDFLAGS) -module -avoid-version
MODULE_LIBADD = $(AM_LIBADD) libpinoscore-@PINOS_MAJORMINOR@.la

###################################
#          Extra files            #
###################################

EXTRA_DIST = \
		daemon/pinos-system.conf \
		daemon/pinos.conf.in \
		daemon/pinos.desktop.in \
		dbus/org.pinos.xml  \
		client/private.h

pinosconf_DATA = \
		daemon/pinos.conf

dbuspolicy_DATA = \
		daemon/pinos-system.conf

xdgautostart_in_files = \
		daemon/pinos.desktop.in
xdgautostart_DATA = $(xdgautostart_in_files:.desktop.in=.desktop)
@INTLTOOL_DESKTOP_RULE@

###################################
#          Includes               #
###################################

enumtypesincludes = client/context.h \
		    client/introspect.h \
		    client/ringbuffer.h \
		    client/stream.h \
		    client/subscribe.h

client/enumtypes.h: $(enumtypesincludes)
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#ifndef __PINOS_ENUM_TYPES_H__\n#define __PINOS_ENUM_TYPES_H__\n\n#include <glib-object.h>\n\nG_BEGIN_DECLS\n" \
	--fprod "\n/* enumerations from \"@filename@\" */\n" \
	--vhead "GType @enum_name@_get_type (void);\n#define PINOS_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\n"         \
	--ftail "G_END_DECLS\n\n#endif /* __PINOS_ENUM_TYPES_H__ */" \
	$^ > client/enumtypes.h

client/enumtypes.c: $(enumtypesincludes) client/enumtypes.h
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#include \"enumtypes.h\"\n#include <pinos/client/pinos.h>\n#define C_ENUM(v) ((gint) v)\n#define C_FLAGS(v) ((guint) v)\n " \
	--fprod "\n/* enumerations from \"@filename@\" */" \
	--vhead "GType\n@enum_name@_get_type (void)\n{\n  static gsize id = 0;\n  static const G@Type@Value values[] = {"     \
	--vprod "    { C_@TYPE@(@VALUENAME@), \"@VALUENAME@\", \"@valuenick@\" }," \
	--vtail "    { 0, NULL, NULL }\n  };\n\n  if (g_once_init_enter (&id)) {\n    GType tmp = g_@type@_register_static (\"@EnumName@\", values);\n    g_once_init_leave (&id, tmp);\n  }\n\n  return (GType) id;\n}" \
	$^ > client/enumtypes.c


dbus/org-pinos.c: dbus/org-pinos.h
dbus/org-pinos.h: dbus/org.pinos.xml
	@$(MKDIR_P) ../doc
	$(AM_V_GEN) $(GDBUS_CODEGEN) \
	--interface-prefix org.pinos. \
	--generate-c-code dbus/org-pinos \
	--generate-docbook ../doc/org-pinos \
	--c-namespace Pinos $< \
        --c-generate-object-manager


built_header_make = client/enumtypes.h dbus/org-pinos.h
built_source_make = client/enumtypes.c dbus/org-pinos.c

BUILT_SOURCES = $(built_header_make)            \
		$(built_source_make)

CLEANFILES = $(built_header_make) $(built_source_make)

###################################
#          Main daemon            #
###################################

bin_PROGRAMS = pinos

pinos_SOURCES = \
		daemon/daemon-config.h daemon/daemon-config.c \
		daemon/main.c

pinos_CFLAGS = $(AM_CFLAGS)
pinos_LDADD = $(AM_LDADD) libpinoscore-@PINOS_MAJORMINOR@.la libpinos-@PINOS_MAJORMINOR@.la $(LIBLTDL)
# This is needed because automake doesn't properly expand the foreach below
pinos_DEPENDENCIES = libpinoscore-@PINOS_MAJORMINOR@.la libpinos-@PINOS_MAJORMINOR@.la
pinos_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS) $(IMMEDIATE_LDFLAGS)

###################################
#         Test programs           #
###################################
noinst_LTLIBRARIES =

TESTS_default =

TESTS_norun = test-client

# These tests need a running pinos daemon
TESTS_daemon =

noinst_PROGRAMS = $(TESTS_default) $(TESTS_norun) $(TESTS_daemon)

test_client_SOURCES = tests/test-client.c
test_client_CFLAGS = $(AM_CFLAGS)
test_client_LDADD = $(AM_LDADD) libpinos-@PINOS_MAJORMINOR@.la
test_client_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Tools programs          #
###################################

bin_PROGRAMS += \
		pinos-monitor

pinos_monitor_SOURCES = tools/pinos-monitor.c
pinos_monitor_CFLAGS = $(AM_CFLAGS)
pinos_monitor_LDADD = $(AM_LDADD) libpinos-@PINOS_MAJORMINOR@.la
pinos_monitor_LDFLAGS = $(AM_LDFLAGS) $(BINLDFLAGS)

###################################
#         Client library          #
###################################

pinosinclude_HEADERS = \
		client/context.h \
		client/enumtypes.h \
		client/format.h \
		client/introspect.h \
		client/mainloop.h \
		client/pinos.h \
		client/properties.h \
		client/stream.h \
		client/subscribe.h

lib_LTLIBRARIES = \
		libpinos-@PINOS_MAJORMINOR@.la

# Public interface
libpinos_@PINOS_MAJORMINOR@_la_SOURCES = \
		client/context.h client/context.c \
		client/enumtypes.h client/enumtypes.c \
		client/format.h client/format.c \
		client/introspect.h client/introspect.c \
		client/mainloop.h client/mainloop.c \
		client/properties.h client/properties.c \
		client/stream.h client/stream.c \
		client/pinos.c client/pinos.h \
		client/ringbuffer.c client/ringbuffer.h \
		client/subscribe.c client/subscribe.h

libpinos_@PINOS_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS)
libpinos_@PINOS_MAJORMINOR@_la_LDFLAGS = $(AM_LDFLAGS) -avoid-version
libpinos_@PINOS_MAJORMINOR@_la_LIBADD = $(AM_LIBADD) $(LTLIBICONV)

###################################
#      Daemon core library        #
###################################

lib_LTLIBRARIES += libpinoscore-@PINOS_MAJORMINOR@.la

# Pure core stuff
libpinoscore_@PINOS_MAJORMINOR@_la_SOURCES = \
		server/client.c server/client.h \
		server/client-node.c server/client-node.h \
		server/command.c server/command.h \
		server/daemon.c server/daemon.h \
		server/link.c server/link.h \
		server/module.c server/module.h \
		server/node.c server/node.h \
		server/node-factory.c server/node-factory.h \
		dbus/org-pinos.c dbus/org-pinos.h

libpinoscore_@PINOS_MAJORMINOR@_la_CFLAGS = $(AM_CFLAGS) $(SERVER_CFLAGS)
libpinoscore_@PINOS_MAJORMINOR@_la_LDFLAGS = $(AM_LDFLAGS) -avoid-version
libpinoscore_@PINOS_MAJORMINOR@_la_LIBADD = $(LIBLTDL) $(LTLIBICONV) \
			 libpinos-@PINOS_MAJORMINOR@.la $(AM_LIBADD)

###################################
#      Modules                    #
###################################

module_LTLIBRARIES =

if BUILD_MODULE_GST
module_LTLIBRARIES += module-gst.la

module_gst_la_SOURCES = \
		modules/gst/gst-manager.c \
		modules/gst/gst-node-factory.c \
		modules/gst/module.c
module_gst_la_CFLAGS = $(AM_CFLAGS) $(GST_CFLAGS)
module_gst_la_LDFLAGS = $(MODULE_LDFLAGS)
module_gst_la_LIBADD = $(MODULE_LIBADD)
endif

if BUILD_MODULE_SPA
module_LTLIBRARIES += module-spa.la

module_spa_la_SOURCES = \
		modules/spa/spa-audiotestsrc.c \
		modules/spa/spa-alsa-monitor.c \
		modules/spa/spa-v4l2-monitor.c \
		modules/spa/spa-videotestsrc.c \
		modules/spa/module.c
module_spa_la_CFLAGS = $(AM_CFLAGS)
module_spa_la_LDFLAGS = $(MODULE_LDFLAGS)
module_spa_la_LIBADD = $(MODULE_LIBADD)
endif




###################################
#      GStreamer Plugin           #
###################################

plugindir = $(libdir)/gstreamer-1.0

plugin_LTLIBRARIES = libgstpinos.la

libgstpinos_la_SOURCES = \
                        gst/gstburstcache.c \
                        gst/gstpinos.c \
                        gst/gstpinosclock.c \
                        gst/gstpinosformat.c \
                        gst/gstpinosdeviceprovider.c \
                        gst/gstpinossrc.c \
                        gst/gstpinossink.c  \
                        gst/gstpinospool.c

libgstpinos_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_BASE_CFLAGS) $(GST_CFLAGS) $(GLIB_CFLAGS)
libgstpinos_la_LDFLAGS = $(AM_LDFLAGS) $(GST_PLUGIN_LDFLAGS)
libgstpinos_la_LIBADD =  $(GST_BASE_LIBS) $(GST_LIBS) $(GLIB_LIBS) $(LIBM) -lgstvideo-1.0 -lgstaudio-1.0 \
			 libpinos-@PINOS_MAJORMINOR@.la libpinoscore-@PINOS_MAJORMINOR@.la $(AM_LIBADD)
libgstpinos_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)

noinst_HEADERS = gst/gstburstcache.h gst/gstpinossrc.h \
		 gst/gstpinosclock.h gst/gstpinosformat.h \
		 gst/gstpinossink.h gst/gstpinosdeviceprovider.h

###################################
#        Some minor stuff         #
###################################

CLEANFILES += daemon/pinos.desktop
DISTCLEANFILES =
INSTALL_DIRS = $(moduledir)

install-directories-hook:
	$(MKDIR_P) $(addprefix $(DESTDIR),$(INSTALL_DIRS))

INSTALL_EXEC_HOOKS = install-directories-hook

install-exec-hook: $(INSTALL_EXEC_HOOKS)

# Automatically generate linker version script. We use the same one for all public .sos
update-map-file:
	( echo "PINOS_0 {" ; \
	  echo "global:" ; \
	  ctags -I PV_GCC_MALLOC,PV_GCC_ALLOC_SIZE2,PV_GCC_ALLOC_SIZE,PV_GCC_PURE,PV_GCC_CONST,PV_GCC_DEPRECATED,PV_GCC_PRINTF_ATTR -f - --c-kinds=p $(pinosinclude_HEADERS) | awk '/^pa_/ { print $$1 ";" }' | sort ; \
	  echo "local:" ;  \
	  echo "*;" ; \
	  echo "};" ) > $(srcdir)/map-file

update-all: update-map-file

.PHONY: update-all update-map-file coverage $(INSTALL_EXEC_HOOKS)
