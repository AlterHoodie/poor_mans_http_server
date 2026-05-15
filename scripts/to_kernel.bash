#!/usr/bin/env bash
set -euo pipefail

PCI_DEV="${PCI_DEV:-0000:03:00.0}"

sudo "dpdk-devbind.py" --bind=r8169 "$PCI_DEV"

echo "Successfully Binded to r8169 kernel driver"
