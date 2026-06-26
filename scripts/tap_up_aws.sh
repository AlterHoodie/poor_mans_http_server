#!/usr/bin/env bash
# AWS TAP setup: bridge a SECONDARY ENI with tap0 for the userspace stack.
#
# Design (two IPs, same subnet):
#   - br0 keeps the ENI's PRIMARY private IP  -> kernel reachability / mgmt.
#   - tap0 (userspace stack) uses a SECONDARY private IP in the same subnet.
#
# AWS only delivers packets to an IP that is registered on an ENI, so the
# secondary stack IP MUST be added to the ENI as a secondary private IP.
#
# Prerequisites (AWS console / CLI):
#   1. Secondary ENI attached to this instance (do NOT use the primary ENI;
#      the primary keeps the kernel networking that SSH rides on).
#   2. A secondary private IP assigned to that ENI:
#        aws ec2 assign-private-ip-addresses \
#          --network-interface-id eni-xxxxxxxx \
#          --private-ip-addresses <STACK_IP>
#   3. Source/dest check DISABLED on that ENI:
#        aws ec2 modify-network-interface-attribute \
#          --network-interface-id eni-xxxxxxxx \
#          --no-source-dest-check
#   4. Security group on the ENI allows inbound ICMP + your server ports.
#
# Usage: sudo ./scripts/tap_up_aws.sh <SECONDARY_ENI> <ENI_PRIMARY_IP/PREFIX> <STACK_IP>
#   e.g. sudo ./scripts/tap_up_aws.sh ens6 172.31.18.161/20 172.31.18.162

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: sudo $0 <secondary-eni> <eni-primary-ip/prefix> <stack-ip>" >&2
    echo "  example: sudo $0 ens6 172.31.18.161/20 172.31.18.162" >&2
    exit 1
fi

ENI="$1"
ENI_CIDR="$2"      # e.g. 172.31.18.161/20  (goes on br0)
STACK_IP="$3"      # e.g. 172.31.18.162     (used by userspace stack on tap0)
TAP="tap0"
BRIDGE="br0"

if [[ $EUID -ne 0 ]]; then
    echo "error: must be run as root (sudo $0)" >&2
    exit 1
fi

if ! ip link show "$ENI" &>/dev/null; then
    echo "error: interface $ENI not found (run: ip -br link)" >&2
    exit 1
fi

ENI_MAC=$(ip link show "$ENI" | awk '/link\/ether/ {print $2}')

echo "==> secondary ENI: $ENI  mac=$ENI_MAC"
echo "==> flushing kernel IPs from $ENI (br0 will own the primary IP)"
ip addr flush dev "$ENI"

echo "==> enabling promiscuous mode on $ENI (needed for bridge on AWS)"
ip link set "$ENI" promisc on

echo "==> creating TAP interface $TAP (cloned MAC: $ENI_MAC)"
ip tuntap add dev "$TAP" mode tap 2>/dev/null || echo "  (already exists, skipping)"
ip link set "$TAP" address "$ENI_MAC"
ip link set "$TAP" up

echo "==> creating bridge $BRIDGE (STP off)"
ip link add "$BRIDGE" type bridge 2>/dev/null || echo "  (already exists, skipping)"
ip link set "$BRIDGE" type bridge stp_state 0
ip link set "$BRIDGE" address "$ENI_MAC"

echo "==> attaching $ENI and $TAP to $BRIDGE"
ip link set "$ENI" master "$BRIDGE"
ip link set "$TAP"  master "$BRIDGE"
ip link set "$BRIDGE" up

echo "==> assigning ENI primary IP $ENI_CIDR to $BRIDGE"
ip addr add "$ENI_CIDR" dev "$BRIDGE" 2>/dev/null || echo "  (already assigned, skipping)"

echo ""
echo "Done. Topology:"
echo "  $ENI  ──┐   (no kernel IP; promisc on)"
echo "           ├── $BRIDGE  (kernel IP: $ENI_CIDR)"
echo "  $TAP  ──┘   (userspace stack IP: $STACK_IP)"
echo ""
echo "Set the STACK IP in src/main.cpp (run_http_server AND run_udp_server):"
echo "  ip4_addr_t ip($(echo "$STACK_IP" | tr '.' ', '));"
echo ""
echo "Make sure $STACK_IP is registered as a secondary private IP on the ENI."
echo ""
echo "Run the server:"
echo "  sudo ./build/server tcp"
echo ""
echo "Test from another instance in the VPC:"
echo "  ping $STACK_IP"
echo "  curl http://$STACK_IP/hi"
