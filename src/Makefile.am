# This is _NOT_ the library release version, it's an API version.
# Please read Chapter 6 "Library interface versions" of the libtool documentation before making any modification
LIBVERSION=1:0:0

INCLUDES = $(all_includes) -I$(top_srcdir)/include -I$(top_builddir)
AM_CFLAGS= -fPIC -Wall $(LIBOSMOCORE_CFLAGS) $(LIBOSMOGSM_CFLAGS) $(LIBOSMOVTY_CFLAGS) $(COVERAGE_CFLAGS)
AM_LDFLAGS = $(LIBOSMOCORE_LIBS) $(LIBOSMOGSM_LIBS) $(LIBOSMOVTY_LIBS) $(COVERAGE_LDFLAGS)

SUBDIRS = channel

lib_LTLIBRARIES = libosmonetif.la

libosmonetif_la_LIBADD = channel/abis/libosmonetif-abis.la
libosmonetif_la_LDFLAGS = -version-info $(LIBVERSION) -no-undefined

libosmonetif_la_SOURCES = amr.c			\
			  channel.c		\
			  datagram.c		\
			  ipa.c			\
			  ipa_unit.c		\
			  osmux.c		\
			  rs232.c		\
			  rtp.c			\
			  stream.c
