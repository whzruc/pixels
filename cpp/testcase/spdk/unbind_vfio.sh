#!/usr/bin/env bash
# Return NVMe controllers from vfio-pci to the kernel and remount filesystems.

set -euo pipefail

STATE_FILE="${SPDK_PCI_STATE_FILE:-/tmp/pixels_spdk_pci_devices}"

if [[ $EUID -ne 0 ]]; then
    echo "[error] run as root: sudo $0" >&2
    exit 1
fi
if [[ ! -s "$STATE_FILE" ]]; then
    echo "[error] missing PCI state file: $STATE_FILE" >&2
    exit 1
fi

mapfile -t DATA_PCI < "$STATE_FILE"
modprobe nvme

for pci in "${DATA_PCI[@]}"; do
    if [[ -e "/sys/bus/pci/drivers/vfio-pci/$pci" ]]; then
        echo "$pci" > /sys/bus/pci/drivers/vfio-pci/unbind
    fi
    echo "" > "/sys/bus/pci/devices/$pci/driver_override"
    echo "$pci" > /sys/bus/pci/drivers/nvme/bind
    echo "[unbind_vfio] $pci -> nvme"
done

echo "[unbind_vfio] waiting for NVMe namespaces..."
sleep 2
udevadm settle || true
mount -a
echo "[unbind_vfio] complete; filesystems restored via /etc/fstab"
