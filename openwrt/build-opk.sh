#!/bin/bash -e

PKG_NAME=asterisk-chan-quectel

if [ -f .config ]; then
    # rebuild
    make package/${PKG_NAME}/clean
else
    # configure SDK first
    cat feeds.conf.default feeds-strskx.conf >> feeds.conf
    ./scripts/feeds update -a
    ./scripts/feeds install -a
    ./scripts/feeds install ${PKG_NAME}
    ./scripts/feeds uninstall linux linux-firmware

    cp diffconfig .config
    make defconfig
fi

make package/${PKG_NAME}/compile "$@"
make package/index

IPK=$(ls bin/packages/*/strskx/*.ipk)
echo "Package: $IPK"
