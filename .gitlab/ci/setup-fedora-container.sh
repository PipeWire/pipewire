#!/bin/sh
set -ex

BLUEZ_COMMIT=$1
PYTEST_BLUEZENV_VERSION=$2
BZIMAGE_URL=$3

COMPILE_ARGS=
if [ -n "$FDO_CI_CONCURRENT" ]; then
    COMPILE_ARGS="-j$FDO_CI_CONCURRENT"
fi

# Build BlueZ snapshot for bluetooth VM testing
git clone --depth 1 --revision "$BLUEZ_COMMIT" https://git.kernel.org/pub/scm/bluetooth/bluez.git /bluez-build
cd /bluez-build
./bootstrap
./configure --enable-tools --disable-obex --enable-asan --enable-ubsan --enable-debug
# shellcheck disable=SC2086
make $COMPILE_ARGS
cd /

#  Store bluetooth VM testing tools
mkdir /bluez /bluez/client /bluez/tools /bluez/src
cp /bluez-build/client/bluetoothctl /bluez/client/
cp /bluez-build/tools/btmgmt /bluez/tools/
cp /bluez-build/src/bluetoothd  /bluez/src/

cd /bluez
python3 -m pip wheel --no-deps "$PYTEST_BLUEZENV_VERSION"
curl -L "$BZIMAGE_URL" -o bzImage
cd /

# Cleanup
rm -rf /bluez-build
