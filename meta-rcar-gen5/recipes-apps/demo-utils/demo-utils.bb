SUMMARY = "tuning app systemd service"
LICENSE = "CLOSED"

SRC_URI += "file://tuning_app.service file://remoteproc.service \
            file://remoteproc_script.sh \
            file://rcar-dsp-rproc.ko \
            file://audio_server.elf \
            file://VpxAweRuntime.elf \
            file://music-sample-48000hz-16bit.wav"

S = "${UNPACKDIR}"

inherit systemd

# Install script and service
do_install() {
    # Install script
    install -d ${D}${bindir}
    install -m 0777 ${S}/remoteproc_script.sh ${D}${bindir}/remoteproc_script

    install -d -m 0755 ${D}${nonarch_base_libdir}/modules
    install -m 0644 ${S}/rcar-dsp-rproc.ko ${D}${nonarch_base_libdir}/modules/rcar-dsp-rproc.ko

    install -d ${D}${ROOT_HOME}/demo_utils
    cp -r ${S}/music-sample-48000hz-16bit.wav ${D}/${ROOT_HOME}/demo_utils

    install -d ${D}${nonarch_base_libdir}/firmware
    install -m 644 ${S}/VpxAweRuntime.elf ${D}${nonarch_base_libdir}/firmware/VpxAweRuntime.elf
    install -m 644 ${S}/audio_server.elf ${D}${nonarch_base_libdir}/firmware/audio_server.elf

    # Install systemd service
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/tuning_app.service ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/remoteproc.service ${D}${systemd_system_unitdir}
}

# Enable systemd service
SYSTEMD_SERVICE:${PN} = "remoteproc.service"
SYSTEMD_SERVICE:${PN} += "tuning_app.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${ROOT_HOME}/demo_utils ${bindir}/remoteproc_script ${systemd_system_unitdir}/remoteproc.service ${ROOT_HOME}/demo_utils/music-sample-48000hz-16bit.wav ${nonarch_base_libdir}/modules/rcar-dsp-rproc.ko ${nonarch_base_libdir}/firmware/audio_server.elf ${nonarch_base_libdir}/firmware/VpxAweRuntime.elf"

INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_PACKAGE_DEBUG_SPLIT = "1"
INSANE_SKIP:${PN} := "arch already-stripped"
