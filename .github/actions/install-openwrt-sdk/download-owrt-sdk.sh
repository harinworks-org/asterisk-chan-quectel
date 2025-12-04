#!/bin/bash -e

# first parameter - target (architecture)
# second parameter - OpenWRT version
# third parameter - SDK name

sd=$(echo $1|cut -d '-' --output-delimiter '/' -f 1-2)
wget -q https://downloads.openwrt.org/releases/$2/targets/${sd}/$3.tar.xz
tar -xf $3.tar.xz
rm $3.tar.xz
mv $3 $1
