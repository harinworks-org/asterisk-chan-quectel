include $(TOPDIR)/rules.mk

PKG_NAME:=asterisk-chan-quectel
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/harinworks-org/asterisk-chan-quectel
PKG_SOURCE_VERSION:=master
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=LICENSE.md
PKG_MAINTAINER:=Harin Lee <me@harin.net>

PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/asterisk-chan-quectel
  SUBMENU:=Telephony
  SECTION:=net
  CATEGORY:=Network
  URL:=https://github.com/harinworks-org/asterisk-chan-quectel
  DEPENDS:=+asterisk +libsqlite3 +alsa-lib
  TITLE:=Quectel module support
endef

define Package/asterisk-chan-quectel/description
  Asterisk channel driver for Quectel and SimCOM modules.
endef

define Package/asterisk-chan-quectel/conffiles
/etc/asterisk/quectel.conf
endef

define Package/asterisk-chan-quectel/install
	$(INSTALL_DIR) $(1)/usr/lib/asterisk/modules
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/asterisk/modules/chan_quectel.so $(1)/usr/lib/asterisk/modules
	$(INSTALL_DIR) $(1)/etc/asterisk
	$(INSTALL_CONF) $(PKG_INSTALL_DIR)/etc/asterisk/quectel.conf $(1)/etc/asterisk
endef

define Package/asterisk-chan-quectel/postinst
#!/bin/sh
if [ -z "$${IPKG_INSTROOT}" ]; then
  echo
  echo 'o-------------------------------------------------------------------o'
  echo '| asterisk-chan-quectel notes                                       |'
  echo 'o-------------------------------------------------------------------o'
  echo '| Adding the "asterisk" user to the "dialout" group might be        |'
  echo '| required for Asterisk to be able to access the module.            |'
  echo 'o-------------------------------------------------------------------o'
  echo '| Adding the "asterisk" user to the "audio" group might be          |'
  echo '| required for Asterisk to be able to access the audio device.      |'
  echo 'o-------------------------------------------------------------=^_^=-o'
  echo
fi
exit 0
endef

$(eval $(call BuildPackage,asterisk-chan-quectel))
