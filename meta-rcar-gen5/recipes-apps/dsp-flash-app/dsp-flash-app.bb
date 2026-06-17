SUMMARY = "DSP flash apps"
DESCRIPTION = "Utilities for flashing dsp core"
LICENSE = "CLOSED"

SRC_URI = "file://dsp_flash"

S = "${WORKDIR}"

do_install() {
	install -d ${D}${ROOT_HOME}/dsp_flash
	cp -r ${WORKDIR}/dsp_flash/* ${D}/home/root/dsp_flash/
		
	# change permissions
	chmod 777 ${D}${ROOT_HOME}/dsp_flash/run_dspss.sh
	chmod 777 ${D}${ROOT_HOME}/dsp_flash/devmemcpy

	# copy dsp elf file to /lib/firmware
	install -d ${D}${nonarch_base_libdir}/firmware
	install -m 644 ${WORKDIR}/dsp_flash/dspss_sample_kernel_cl0_c0_x5h.elf ${D}${nonarch_base_libdir}/firmware/dspss_sample_kernel_cl0_c0_x5h.elf

}

INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_PACKAGE_DEBUG_SPLIT = "1"
FILES:${PN} := "${ROOT_HOME}/dsp_flash ${nonarch_base_libdir}/firmware/dspss_sample_kernel_cl0_c0_x5h.elf"
INSANE_SKIP:${PN} := "arch already-stripped"
