RENESAS_BSP_URL = " \
    git@github.com:Ignitarium-Software/linux-bsp-nr.git;protocol=https"

RENESAS_BSP_BRANCH = "${@oe.utils.conditional("MACHINE", "x5h", \
    "v6.1.102/rcar-5.2.0.rc11_vpf.rc25", \
    "feature/audio_server_integration", d)}"
>>>>>>> c2d0175d (meta-rcar-gen5: include: update kernel repo to neurealm repo)
RENESAS_BSP_SRCREV = "${@oe.utils.conditional("MACHINE", "x5h", \
    "b88700836be3c18ee4f8b30d18e18150571c317d", \
    "17bbfab8a496df23f2a412cda0ccc8226f787269", d)}"

# Renesas Yocto BSP version
BSP_RENESAS_VERSION = "Renesas-Yocto-v7.26.0"

# Renesas SDK version
SDK_VERSION = "v4.36.0"
