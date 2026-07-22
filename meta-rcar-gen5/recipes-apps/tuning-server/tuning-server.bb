SUMMARY = "Linux tuning server"
DESCRIPTION = "Receives Audio Weaver data over Ethernet and forwards via RPMsg"
LICENSE = "MIT"

LIC_FILES_CHKSUM = "file://LICENSE;md5=ff953ea2c560df4b9b2ae41b98063727"

SRC_URI = "file://src/main.c \
           file://src/awe_pkt_mngt.c \
           file://src/file_ops.c \
           file://src/tcp_app.c \
           file://inc/tuning_server.h \
           file://CMakeLists.txt \
           file://LICENSE"

S = "${UNPACKDIR}"

inherit cmake

# Optional: pass extra flags
EXTRA_OECMAKE = ""
