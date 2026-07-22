RENESAS_BSP_URL = " \
    git://github.com/Ignitarium-Software/linux-bsp.git;protocol=https"

RENESAS_BSP_BRANCH = "${@oe.utils.conditional("MACHINE", "x5h", \
    "v6.1.102/rcar-5.2.0.rc11_vpf.rc25", \
    "rebase/v4.36.0", d)}"
RENESAS_BSP_SRCREV = "${@oe.utils.conditional("MACHINE", "x5h", \
    "b88700836be3c18ee4f8b30d18e18150571c317d", \
    "c3fc0ab7bd1901c8d24d40e8951fd7dd8e9a13ee", d)}"

# Renesas Yocto BSP version
BSP_RENESAS_VERSION = "Renesas-Yocto-v7.26.0"

# Renesas SDK version
SDK_VERSION = "v4.36.0"
