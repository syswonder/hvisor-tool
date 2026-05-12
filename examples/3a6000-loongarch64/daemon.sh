#!/bin/bash
# Usage: daemon.sh [virtio_cfg.json]
# If no argument given, defaults to virtio_cfg.json

mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

cfg="${1:-virtio_cfg.json}"
echo "Starting virtio with config: $cfg"
nohup hvisor virtio start "$cfg" &