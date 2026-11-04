################################################################################
#
# mpfr
#
################################################################################

MPFR_VERSION = 4.0.2
MPFR_SITE = http://www.mpfr.org/mpfr-$(MPFR_VERSION)
MPFR_SOURCE = mpfr-$(MPFR_VERSION).tar.xz
MPFR_LICENSE = LGPL-3.0+
MPFR_LICENSE_FILES = COPYING.LESSER
MPFR_INSTALL_STAGING = YES
MPFR_DEPENDENCIES = gmp
HOST_MPFR_DEPENDENCIES = host-gmp
MPFR_MAKE_OPTS = RANLIB=$(TARGET_RANLIB)

# Older MPFR configure probes rely on pre-C23 empty parameter lists.
HOST_MPFR_CONF_ENV = CFLAGS="$(HOST_CFLAGS) -std=gnu17"
HOST_MPFR_MAKE_OPTS = CFLAGS="$(HOST_CFLAGS) -std=gnu17"

$(eval $(autotools-package))
$(eval $(host-autotools-package))
