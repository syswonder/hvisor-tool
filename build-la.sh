#!/bin/bash
# Build hvisor-tool for LoongArch64.
# Usage: bash build-la.sh <linux-kernel-path> [extra make args...]

set -e

SCRIPT_DIR=$(dirname "$(realpath "$0")")

KDIR=${1:-/media/boneinscri/Data/linux-v6.19/linux}
shift 2>/dev/null || true

if [ ! -d "$KDIR" ]; then
    echo "Error: kernel directory not found: $KDIR"
    exit 1
fi

if [ ! -f "$KDIR/Makefile" ]; then
    echo "Error: $KDIR does not look like a kernel source tree (no Makefile)"
    exit 1
fi

echo "Building hvisor-tool for loongarch64"
echo "  KDIR = $KDIR"

make -C "$SCRIPT_DIR" \
    ARCH=loongarch \
    KDIR="$KDIR" \
    CROSS_COMPILE=loongarch64-linux-gnu- \
    LOG=LOG_INFO \
    "$@"
