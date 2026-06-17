#!/usr/bin/env bash
# Set up tap0 + br0 and attach the physical NIC so the userspace stack
# can reach the local network.
#
# Usage: sudo ./scripts/tap_up.sh [NIC]
#   NIC defaults to enp3s0

set -euo pipefail

NIC="${1:-enp3s0}"
TAP="tap0"
BRIDGE="br0"

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root (sudo $0)" >&2
    exit 1
fi

echo "==> creating TAP interface $TAP"
ip tuntap add dev "$TAP" mode tap 2>/dev/null || echo "  (already exists, skipping)"
ip link set "$TAP" up

echo "==> creating bridge $BRIDGE"
ip link add "$BRIDGE" type bridge 2>/dev/null || echo "  (already exists, skipping)"

echo "==> attaching $NIC and $TAP to $BRIDGE"
ip link set "$NIC"  master "$BRIDGE"
ip link set "$TAP"  master "$BRIDGE"
ip link set "$BRIDGE" up

echo ""
echo "Done. Network topology:"
echo "  $NIC  ──┐"
echo "           ├── $BRIDGE (bridge)"
echo "  $TAP  ──┘"
echo ""
echo "Run the server with:"
echo "  sudo ./build/server tcp"
echo "  sudo ./build/server udp echo"
