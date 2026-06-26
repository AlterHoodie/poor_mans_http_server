#!/usr/bin/env bash
# AWS TAP setup: bridge a secondary ENI with tap0 for the userspace stack.
#
# Keep the primary ENI (ens5/eth0) on kernel networking for SSH.
# Dedicate the secondary ENI to tap0 + br0.
#
# Prerequisites (AWS console / CLI):
#   1. Secondary ENI attached to this instance
#   2. Source/dest check DISABLED on that ENI:
#        aws ec2 modify-network-interface-attribute \
#          --network-interface-id eni-xxxxxxxx \
#          --no-source-dest-check
#   3. Security group on the ENI allows inbound ICMP + your server ports
#
# Usage: sudo ./scripts/tap_up_aws.sh <SECONDARY_ENI>
#   e.g. sudo ./scripts/tap_up_aws.sh ens6

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: sudo $0 <secondary-eni>" >&2
    echo "  example: sudo $0 ens6" >&2
    exit 1
fi

ENI="$1"
TAP="tap0"
BRIDGE="br0"

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root (sudo $0)" >&2
    exit 1
fi

if ! ip link show "$ENI" &>/dev/null; then
    echo "error: interface $ENI not found" >&2
    echo "  run: ip -br link" >&2
    exit 1
fi

ENI_MAC=$(ip link show "$ENI" | awk '/link\/ether/ {print $2}')
ENI_IP=$(ip -4 -o addr show dev "$ENI" | awk '{print $4}' | cut -d/ -f1 | head -1)

echo "==> secondary ENI: $ENI  mac=$ENI_MAC  ip=${ENI_IP:-<none>}"
echo "==> flushing kernel IPv4/IPv6 from $ENI (stack owns the IP in userspace)"
ip addr flush dev "$ENI"

echo "==> enabling promiscuous mode on $ENI (needed for bridge on AWS)"
ip link set "$ENI" promisc on

echo "==> creating TAP interface $TAP"
ip tuntap add dev "$TAP" mode tap 2>/dev/null || echo "  (already exists, skipping)"
ip link set "$TAP" address "$ENI_MAC"
ip link set "$TAP" up

echo "==> creating bridge $BRIDGE"
ip link add "$BRIDGE" type bridge 2>/dev/null || echo "  (already exists, skipping)"

echo "==> attaching $ENI and $TAP to $BRIDGE"
ip link set "$ENI" master "$BRIDGE"
ip link set "$TAP"  master "$BRIDGE"
ip link set "$BRIDGE" up

echo ""
echo "Done. Topology:"
echo "  $ENI  ──┐   (no kernel IP; promisc on)"
echo "           ├── $BRIDGE"
echo "  $TAP  ──┘   (MAC copied from ENI)"
echo ""
if [[ -n "${ENI_IP:-}" ]]; then
    echo "Set this IP in src/main.cpp (both run_http_server and run_udp_server):"
    echo "  ip4_addr_t ip($(echo "$ENI_IP" | tr '.' ', '));"
    echo ""
fi
echo "Run the server:"
echo "  sudo ./build/server tcp"
echo ""
echo "Test from another instance in the VPC:"
if [[ -n "${ENI_IP:-}" ]]; then
    echo "  ping $ENI_IP"
    echo "  curl http://$ENI_IP/hi"
else
    echo "  ping <secondary-eni-private-ip>"
    echo "  curl http://<secondary-eni-private-ip>/hi"
fi
