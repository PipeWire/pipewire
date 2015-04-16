# This file is part of PulseAudio.
#
# PulseAudio is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# PulseAudio is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.

ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = \
	bootstrap.sh \
	git-version-gen \
	LICENSE \
	GPL \
	LGPL \
	PROTOCOL \
	README 

SUBDIRS = src

DIST_SUBDIRS = pkgconfig \
	       doc \
	       man \
	       po \
	       src

MAINTAINERCLEANFILES =
noinst_DATA =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = pkgconfig/libpulsevideo.pc

eolspace:
	find \( -name '*.c' -o -name '*.h' -o -name 'Makefile.am' \) -exec perl -i -pe 's/\s+\n$$/\1\n/;' \{\} \;

untabify:
	find \( -name '*.c' -o -name '*.h' \) -exec perl -i -pe 's/\t/        /g;' \{\} \;

fedora-snapshot: dist
	cp $(distdir).tar.gz $$HOME/git.fedora/pulseaudio/$(distdir).tar.gz

dist-hook:
	echo $(VERSION) > $(distdir)/.tarball-version
	echo $(VERSION) > $(distdir)/.version

.PHONY: distcleancheck

# see git-version-gen
BUILT_SOURCES = $(top_srcdir)/.version
$(top_srcdir)/.version:
	echo $(VERSION) > $@-t && mv $@-t $@

DISTCLEANFILES = \
        po/.intltool-merge-cache
