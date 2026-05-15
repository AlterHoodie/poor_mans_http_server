#!/usr/bin/env bash
sudo modprobe vfio-pci

sudo sysctl -w vm.nr_hugepages=512
