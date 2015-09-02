DISTCLEANFILES = \
	$(noinst_DATA)

noinst_DATA = \
	pinos.1.xml \
	pinos-monitor.1.xml

xmllint: $(noinst_DATA)
	for f in $(noinst_DATA) ; do \
			xmllint --noout --valid "$$f" || exit 1 ; \
	done

if BUILD_MANPAGES

dist_man_MANS = \
	pinos.1 \
	pinos-monitor.1

CLEANFILES = \
	$(dist_man_MANS)

%: %.xml Makefile
	$(AM_V_GEN) perl $(srcdir)/xmltoman $< > $@ || rm -f $@

endif

EXTRA_DIST = \
	pinos.1.xml.in \
	pinos-monitor.1.xml.in \
	xmltoman \
	xmltoman.css \
	xmltoman.xsl \
	xmltoman.dtd

