#!/usr/bin/env bash
# Tear down tap0 and br0, restoring the physical NIC to standalone mode.
#
# Usage: sudo ./scripts/tap_down.sh [NIC]
#   NIC defaults to enp3s0

set -euo pipefail

NIC="${1:-enp3s0}"
TAP="tap0"
BRIDGE="br0"

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root (sudo $0)" >&2
    exit 1
fi

echo "==> detaching $NIC from $BRIDGE"
ip link set "$NIC" nomaster 2>/dev/null || echo "  ($NIC was not a bridge member)"

echo "==> detaching $TAP from $BRIDGE"
ip link set "$TAP" nomaster 2>/dev/null || echo "  ($TAP was not a bridge member)"

echo "==> removing bridge $BRIDGE"
ip link set "$BRIDGE" down 2>/dev/null || true
ip link delete "$BRIDGE" type bridge 2>/dev/null || echo "  ($BRIDGE did not exist)"

echo "==> removing TAP interface $TAP"
ip link set "$TAP" down 2>/dev/null || true
ip tuntap del dev "$TAP" mode tap 2>/dev/null || echo "  ($TAP did not exist)"

echo ""
echo "Done. $NIC is back to standalone mode."
