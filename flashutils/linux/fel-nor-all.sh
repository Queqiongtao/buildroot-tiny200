#!/bin/bash
set -e
SELFDIR=$(dirname "$(realpath "$0")")
ROOTDIR=$(realpath "${SELFDIR}/../..")
XFELL="${SELFDIR}/bin/xfel"
IMAGE="${1:-${ROOTDIR}/output/images/sysimage-nor.img}"

[ -f "${IMAGE}" ] || {
    echo "SYNTAX: $0 [image]"
    echo "Image not found: ${IMAGE}"
    exit 1
}

echo -n "Waiting for FEL Device"
until "${XFELL}" version 2>/dev/null | grep -q "AWUSBFEX"; do
    echo -n "."
    sleep 1
done
echo " Gotcha!"

"${XFELL}" spinor erase 0x0 0x1000000
"${XFELL}" spinor write 0x0 "${IMAGE}"
"${XFELL}" reset
