SUMMARY = "tuning app systemd service"
LICENSE = "CLOSED"

SRC_URI += "file://tuning_app.service file://remoteproc.service \
            file://remoteproc_script.sh \
            file://rcar-dsp-rproc.ko \
            file://music-sample-48000hz-16bit.wav"

S = "${WORKDIR}"

inherit systemd

# Install script and service
do_install() {
    # Install script
    install -d ${D}${bindir}
    install -m 0777 ${S}/remoteproc_script.sh ${D}${bindir}/remoteproc_script

    install -d ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}
    install -m 0644 ${S}/rcar-dsp-rproc.ko ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/

    install -d ${D}${ROOT_HOME}/demo_utils
    cp -r ${S}/music-sample-48000hz-16bit.wav ${D}/home/root/demo_utils

    # Install systemd service
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/tuning_app.service ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/remoteproc.service ${D}${systemd_system_unitdir}
}

# Enable systemd service
SYSTEMD_SERVICE:${PN} = "remoteproc.service"
SYSTEMD_SERVICE:${PN} += "tuning_app.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${ROOT_HOME}/dsp_flash ${bindir}/remoteproc_script ${systemd_system_unitdir}/remoteproc.service"
