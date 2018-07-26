AM_CFLAGS = \
	$(WARN_CFLAGS)\
	-I $(top_srcdir)/include/drm \
	-I $(top_srcdir)/libkms/ \
	-I $(top_srcdir)/rockchip \
	-I $(top_srcdir)

if HAVE_INSTALL_TESTS
 bin_PROGRAMS = \
	rockchip_rga_test
else
noinst_PROGRAMS = \
	rockchip_rga_test
endif

rockchip_rga_test_LDADD = \
	$(top_builddir)/libdrm.la \
	$(top_builddir)/rockchip/libdrm_rockchip.la

rockchip_rga_test_SOURCES = \
	rockchip_rga_test.c

