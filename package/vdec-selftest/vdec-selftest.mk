################################################################################
#
# vdec-selftest
#
################################################################################

VDEC_SELFTEST_VERSION = da86dacbb492e3df4c5e10d601cf140d8cf622a9
VDEC_SELFTEST_SITE = $(TOPDIR)/package/vdec-selftest/src
VDEC_SELFTEST_SITE_METHOD = local
VDEC_SELFTEST_LICENSE = GPL-3.0+
VDEC_SELFTEST_LICENSE_FILES = COPYING
VDEC_SELFTEST_DEPENDENCIES = libdrm

VDEC_SELFTEST_SRCS = \
	$(@D)/vdec_selftest.c \
	$(@D)/vdec/mp4_demux.c \
	$(@D)/vdec/bitreader.c \
	$(@D)/vdec/nalu.c \
	$(@D)/vdec/h264_parser.c \
	$(@D)/vdec/h264_dpb.c \
	$(@D)/vdec/vdec_v4l2.c

define VDEC_SELFTEST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -D_GNU_SOURCE \
		-I$(@D) -I$(@D)/vdec \
		-I$(STAGING_DIR)/usr/include/libdrm \
		-o $(@D)/vdec_selftest \
		$(VDEC_SELFTEST_SRCS) \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -ldrm
endef

define VDEC_SELFTEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/vdec_selftest \
		$(TARGET_DIR)/usr/bin/vdec_selftest
endef

$(eval $(generic-package))
