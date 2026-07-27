SUMMARY = "ALSA test App"
DESCRIPTION = "ALSA test Application"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://main.c;md5=b40f03b14ad10b2cf13a48f4d940d6c2"

SRC_URI = "file://main.c"

S = "${UNPACKDIR}"

DEPENDS = "alsa-lib"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} main.c -o alsa_app -lpthread -lasound
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 alsa_app ${D}${bindir}
}
