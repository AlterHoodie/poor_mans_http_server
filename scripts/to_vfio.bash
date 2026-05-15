#!/usr/bin/env bash
set -euo pipefail

PCI_DEV="${PCI_DEV:-0000:03:00.0}"

sudo ip link set enp3s0 down
sudo "dpdk-devbind.py" --bind=vfio-pci "$PCI_DEV"

echo "Successfully Binded to vfio-pci"
