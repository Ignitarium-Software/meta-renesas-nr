RENESAS_BSP_URL = " \
    git://github.com/sourav-ign/linux-bsp;protocol=https"

RENESAS_BSP_BRANCH = "${@oe.utils.conditional("MACHINE", "x5h", \
    "v6.1.102/rcar-5.2.0.rc11_vpf.rc25", \
    "feature/ALSA", d)}"
RENESAS_BSP_SRCREV = "${@oe.utils.conditional("MACHINE", "x5h", \
    "77bb93f28b7d1127c2b64dc9a01f32aa84338dd9", \
    "de4d644f855efafe711a0ab228def10d0eeb97ff", d)}"

# Renesas Yocto BSP version
BSP_RENESAS_VERSION = "Renesas-Yocto-v7.23.0"

# Renesas SDK version
SDK_VERSION = "v4.33.0"
