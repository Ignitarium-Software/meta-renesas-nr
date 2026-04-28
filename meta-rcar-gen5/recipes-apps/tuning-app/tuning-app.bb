SUMMARY = "tuning app systemd service"
LICENSE = "CLOSED"

SRC_URI += "file://tuning_app.service file://remoteproc.service file://remoteproc_script.sh"

S = "${WORKDIR}"

inherit systemd

# Install script and service
do_install() {
    # Install script
     install -d ${D}${bindir}
     install -m 0777 ${WORKDIR}/remoteproc_script.sh ${D}${bindir}/remoteproc_script

    # Install systemd service
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/tuning_app.service ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/remoteproc.service ${D}${systemd_system_unitdir}
}

# Enable systemd service
SYSTEMD_SERVICE:${PN} = "remoteproc.service"
SYSTEMD_SERVICE:${PN} += "tuning_app.service"
SYSTEMD_AUTO_ENABLE = "enable"

FILES:${PN} += "${bindir}/remoteproc_script ${systemd_system_unitdir}/remoteproc.service"
