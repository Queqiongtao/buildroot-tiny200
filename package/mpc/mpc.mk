################################################################################
#
# mpc
#
################################################################################

MPC_VERSION = 1.1.0
MPC_SITE = $(BR2_GNU_MIRROR)/mpc
MPC_LICENSE = LGPL-3.0+
MPC_LICENSE_FILES = COPYING.LESSER
MPC_INSTALL_STAGING = YES
MPC_DEPENDENCIES = gmp mpfr
HOST_MPC_DEPENDENCIES = host-gmp host-mpfr

# Older MPC configure probes rely on pre-C23 empty parameter lists.
HOST_MPC_CONF_ENV = CFLAGS="$(HOST_CFLAGS) -std=gnu17"
HOST_MPC_MAKE_OPTS = CFLAGS="$(HOST_CFLAGS) -std=gnu17"

$(eval $(autotools-package))
$(eval $(host-autotools-package))
