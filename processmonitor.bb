DESCRIPTION = "Linux process monitor - track and record the execution times of all processes"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5="f105654e5d0696b9f9c0fb345979402d"

S = "${WORKDIR}/git"
SRC_URI = "git://github.com/rdkcentral/process-monitor.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"

inherit cmake systemd

do_install_append () {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/process-monitor.service ${D}${systemd_unitdir}/system
}

SYSTEMD_SERVICE_${PN} = "process-monitor.service"

FILES_${PN} += "${systemd_system_unitdir}/process-monitor.service"
FILES_${PN} += "${bindir}/ProcessMonitor"
