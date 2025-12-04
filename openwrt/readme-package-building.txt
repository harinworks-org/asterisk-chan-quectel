OpenWRT package building procedure:

1. Download and extract OpenWRT SDK appropriate to your architecture.
2. Copy feeds-custom.conf, diffconfig and build-opk.sh files to SDK directory.
3. Go to SDK directory.
4. Run ./build-opk.sh script. You may invoke this script multiple times.
5. Package is created into bin/packages/*/custom directory.

Example:

wget https://downloads.openwrt.org/releases/23.05.3/targets/x86/64/openwrt-sdk-23.05.3-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz
tar -xf openwrt-sdk-23.05.3-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz
rm openwrt-sdk-23.05.3-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz
cp feeds-custom.conf diffconfig build-opk.sh openwrt-sdk-23.05.3-x86-64_gcc-12.3.0_musl.Linux-x86_64
cd openwrt-sdk-23.05.3-x86-64_gcc-12.3.0_musl.Linux-x86_64
./build-opk.sh
ls bin/packages/*/custom/*.ipk

Links:

* https://openwrt.org/docs/guide-developer/toolchain/using_the_sdk
