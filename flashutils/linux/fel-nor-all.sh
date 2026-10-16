#!/bin/bash
set -e
SELFDIR=$(dirname "$(realpath "$0")")
ROOTDIR=$(realpath "${SELFDIR}/../..")
XFELL="${SELFDIR}/bin/xfel"
SUNXIFEL="${SELFDIR}/bin/sunxi-fel"
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

"${SUNXIFEL}" -p spiflash-write 0 "${IMAGE}"
"${XFELL}" reset
