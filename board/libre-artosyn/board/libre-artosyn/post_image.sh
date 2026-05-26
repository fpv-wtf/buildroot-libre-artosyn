#!/bin/sh
set -e

BINARIES_DIR="$1"

CFG="${BINARIES_DIR}/ubinize.cfg"

cat > "${CFG}" <<EOF
[ubi-volume]
mode=ubi
image=${BINARIES_DIR}/rootfs.squashfs
vol_id=0
vol_type=dynamic
vol_name=userapp
vol_flags=autoresize
EOF

ubinize \
  -o "${BINARIES_DIR}/userapp0.img" \
  -m 2048 \
  -p 128KiB \
  -O 2048 \
  "${CFG}"

echo "Built userapp0.img"