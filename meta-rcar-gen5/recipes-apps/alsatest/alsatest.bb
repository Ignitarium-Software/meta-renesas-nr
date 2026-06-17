SUMMARY = "ALSA test App"
DESCRIPTION = "ALSA test Application"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://main.c;md5=cd8446fe806657438d7f97e9729b7af4"

SRC_URI = "file://main.c"

S = "${WORKDIR}"

DEPENDS = "alsa-lib"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} main.c -o alsa_app -lpthread -lasound
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 alsa_app ${D}${bindir}
}
