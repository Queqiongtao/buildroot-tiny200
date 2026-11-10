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
VDEC_SELFTEST_DEPENDENCIES = libdrm tinyalsa faad2

VDEC_COMMON_SRCS = \
	$(@D)/vdec/mp4_demux.c \
	$(@D)/vdec/bitreader.c \
	$(@D)/vdec/nalu.c \
	$(@D)/vdec/h264_parser.c \
	$(@D)/vdec/h264_dpb.c \
	$(@D)/vdec/vdec_v4l2.c

VDEC_SELFTEST_SRCS = \
	$(@D)/vdec_selftest.c \
	$(VDEC_COMMON_SRCS)

MP4PLAY_SRCS = \
	$(@D)/mp4play.c \
	$(VDEC_COMMON_SRCS) \
	$(@D)/vdec/mp4_audio_demux.c

define VDEC_SELFTEST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -D_GNU_SOURCE \
		-I$(@D) -I$(@D)/vdec \
		-I$(STAGING_DIR)/usr/include \
		-I$(STAGING_DIR)/usr/include/libdrm \
		-o $(@D)/vdec_selftest \
		$(VDEC_SELFTEST_SRCS) \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib -ldrm
	$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -D_GNU_SOURCE \
		-I$(@D) -I$(@D)/vdec \
		-I$(STAGING_DIR)/usr/include \
		-I$(STAGING_DIR)/usr/include/libdrm \
		-o $(@D)/mp4play \
		$(MP4PLAY_SRCS) \
		$(TARGET_LDFLAGS) -L$(STAGING_DIR)/usr/lib \
		-ldrm -ltinyalsa -lfaad -lpthread -lm
endef

define VDEC_SELFTEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/vdec_selftest \
		$(TARGET_DIR)/usr/bin/vdec_selftest
	$(INSTALL) -D -m 0755 $(@D)/mp4play \
		$(TARGET_DIR)/usr/bin/mp4play
endef

$(eval $(generic-package))
