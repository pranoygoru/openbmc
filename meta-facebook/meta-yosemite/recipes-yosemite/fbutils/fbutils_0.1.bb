# Copyright 2014-present Facebook. All Rights Reserved.
#
# This program file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program in a file named COPYING; if not, write to the
# Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor,
# Boston, MA 02110-1301 USA
SUMMARY = "Utilities"
DESCRIPTION = "Various utilities"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=eb723b61539feef013de476e68b5c50a"

SRC_URI = "file://ast-functions \
           file://us_console.sh \
           file://sol-util \
           file://power_led.sh \
           file://power_util.py \
           file://post_led.sh \
           file://reset_usb.sh \
           file://setup-gpio.sh \
           file://setup_rov.sh \
           file://mdio.py \
           file://bcm5396.py \
           file://bcm5396_util.py \
           file://eth0_mac_fixup.sh \
           file://yosemite_power.sh \
           file://power-on.sh \
           file://wedge_us_mac.sh \
           file://setup_switch.py \
           file://create_vlan_intf \
           file://watch-fc.sh \
           file://fcswitcher.sh \
           file://fw_env_config.sh \
           file://COPYING \
          "

pkgdir = "utils"

S = "${WORKDIR}"

binfiles = "us_console.sh sol-util power_led.sh post_led.sh \
  reset_usb.sh mdio.py setup_rov.sh yosemite_power.sh wedge_us_mac.sh \
  bcm5396.py bcm5396_util.py setup_switch.py watch-fc.sh power_util.py"

DEPENDS_append = "update-rc.d-native"

do_install() {
  dst="${D}/usr/local/fbpackages/${pkgdir}"
  install -d $dst
  install -m 644 ast-functions ${dst}/ast-functions
  localbindir="${D}/usr/local/bin"
  install -d ${localbindir}
  for f in ${binfiles}; do
      install -m 755 $f ${dst}/${f}
      ln -s ../fbpackages/${pkgdir}/${f} ${localbindir}/${f}
  done

  # init
  install -d ${D}${sysconfdir}/init.d
  install -d ${D}${sysconfdir}/rcS.d
  install -m 0755 ${WORKDIR}/fw_env_config.sh ${D}${sysconfdir}/init.d/fw_env_config.sh
  update-rc.d -r ${D} fw_env_config.sh start 05 S .
  install -m 755 setup-gpio.sh ${D}${sysconfdir}/init.d/setup-gpio.sh
  update-rc.d -r ${D} setup-gpio.sh start 59 5 .
  # create VLAN intf automatically
  #install -d ${D}/${sysconfdir}/network/if-up.d
  #install -m 755 create_vlan_intf ${D}${sysconfdir}/network/if-up.d/create_vlan_intf
  # networking is done after rcS, any start level within rcS
  # for mac fixup should work
  #install -m 755 eth0_mac_fixup.sh ${D}${sysconfdir}/init.d/eth0_mac_fixup.sh
  #update-rc.d -r ${D} eth0_mac_fixup.sh start 70 S .
  install -m 755 power-on.sh ${D}${sysconfdir}/init.d/power-on.sh
  update-rc.d -r ${D} power-on.sh start 70 5 .
  #install -m 755 fcswitcher.sh ${D}${sysconfdir}/init.d/fcswitcher.sh
  #update-rc.d -r ${D} fcswitcher.sh start 90 S .
}

FILES_${PN} += "/usr/local ${sysconfdir}"
