#!/usr/bin/env bash
# Unmount Pixels data disks and bind their NVMe controllers to vfio-pci.
# Generate the LBA map before running this script.

set -euo pipefail

STATE_FILE="${SPDK_PCI_STATE_FILE:-/tmp/pixels_spdk_pci_devices}"
MOUNT_GLOB="${SPDK_MOUNT_GLOB:-/data/9a3-*}"

if [[ $EUID -ne 0 ]]; then
    echo "[error] run as root: sudo $0" >&2
    exit 1
fi

mapfile -t MOUNTS < <(findmnt -rn -o TARGET | grep -E '^/data/9a3-[0-9]+$' | sort -V)
if [[ ${#MOUNTS[@]} -eq 0 ]]; then
    echo "[error] no mounted data disks matched $MOUNT_GLOB" >&2
    exit 1
fi

declare -A SEEN=()
DATA_PCI=()
for mountpoint_path in "${MOUNTS[@]}"; do
    source_dev=$(findmnt -n -o SOURCE --target "$mountpoint_path")
    partition=$(basename "$(readlink -f "$source_dev")")
    namespace=$(lsblk -n -o PKNAME "/dev/$partition" | head -n1)
    [[ -n "$namespace" ]] || namespace="$partition"
    device_path=$(readlink -f "/sys/class/block/$namespace/device")
    pci=$(grep -oE '[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]' <<<"$device_path" | tail -n1)
    if [[ -z "$pci" ]]; then
        echo "[error] cannot resolve PCI address for $source_dev" >&2
        exit 1
    fi
    if [[ -z "${SEEN[$pci]:-}" ]]; then
        DATA_PCI+=("$pci")
        SEEN[$pci]=1
    fi
done

printf '%s\n' "${DATA_PCI[@]}" > "$STATE_FILE.tmp"
mv "$STATE_FILE.tmp" "$STATE_FILE"
echo "[bind_vfio] saved ${#DATA_PCI[@]} PCI addresses to $STATE_FILE"

for mountpoint_path in "${MOUNTS[@]}"; do
    umount "$mountpoint_path"
    echo "[bind_vfio] unmounted $mountpoint_path"
done

modprobe vfio-pci
if [[ -w /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
    echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode || true
fi

for pci in "${DATA_PCI[@]}"; do
    if [[ -e "/sys/bus/pci/drivers/nvme/$pci" ]]; then
        echo "$pci" > /sys/bus/pci/drivers/nvme/unbind
    fi
    echo vfio-pci > "/sys/bus/pci/devices/$pci/driver_override"
    echo "$pci" > /sys/bus/pci/drivers/vfio-pci/bind
    echo "[bind_vfio] $pci -> vfio-pci"
done

echo "[bind_vfio] complete; restore with testcase/spdk/unbind_vfio.sh"
