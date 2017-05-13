SUMMARY = "Bluetooth Audio ALSA Backend"
HOMEPAGE = "https://github.com/Arkq/bluez-alsa"
SECTION = "devel"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=bb3e99e80c5d718213f35ae1def4c106"

DEPENDS = "libortp alsa-lib bluez5 glib-2.0 sbc"

SRCREV = "414c5575e76c827d36a020e065d54aa2a1f41ba1"
SRC_URI = "git://github.com/Arkq/bluez-alsa.git;branch=master;protocol=https \
           file://bluez-alsa.service"

FILESEXTRAPATHS_append := "${THISDIR}/files:"
           
S = "${WORKDIR}/git"

inherit systemd pkgconfig autotools

do_install () {
    autotools_do_install
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/bluez-alsa.service ${D}${systemd_unitdir}/system
}

FILES_${PN} += "${libdir}/alsa-lib/lib*.so ${datadir}/alsa"
FILES_${PN}-dev += "${libdir}/alsa-lib/*.la"
FILES_${PN}-staticdev += "${libdir}/alsa-lib/lib*.a"
FILES_${PN}-dbg += "${libdir}/alsa-lib/.debug/*.so"

SYSTEMD_SERVICE_${PN} = "bluez-alsa.service"
