#!/usr/bin/env bash
# 最小复现脚本：VFIO 绑定 → doublebuffer 跑 q45 → 保留 core dump
set -euo pipefail

[[ "$EUID" -ne 0 ]] && { echo "[error] 需要 root: sudo -E bash $0"; exit 1; }

BINARY="${DUCKDB_BINARY:-/home/whz/test/pixels/cpp/build/spdk-release/duckdb.bin}"
SQL_FILE="${SQL_FILE:-/tmp/repro_doublebuffer.sql}"
CORE_DIR="${CORE_DIR:-/tmp}"
PROPERTIES="${PROPERTIES:-/home/whz/opt/pixels/etc/pixels-cpp.properties}"

DATA_PCI=(
    0000:c2:00.0  0000:c3:00.0  0000:c4:00.0  0000:c5:00.0
    0000:81:00.0  0000:82:00.0  0000:83:00.0  0000:84:00.0
    0000:85:00.0  0000:86:00.0  0000:87:00.0  0000:88:00.0
    0000:41:00.0  0000:42:00.0  0000:43:00.0  0000:44:00.0
    0000:45:00.0  0000:46:00.0  0000:47:00.0  0000:48:00.0
    0000:01:00.0  0000:02:00.0  0000:03:00.0  0000:04:00.0
)

VFIO_BOUND=0

bind_vfio() {
    echo "[bind_vfio] 卸载数据盘..."
    for i in $(seq -w 1 24); do
        mnt="/data/9a3-$i"
        mountpoint -q "$mnt" 2>/dev/null && umount "$mnt" && echo "  umount $mnt OK" || true
    done
    modprobe vfio-pci
    [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]] && \
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true
    echo "[bind_vfio] 绑定 vfio-pci..."
    for pci in "${DATA_PCI[@]}"; do
        [[ -e "/sys/bus/pci/drivers/nvme/$pci" ]] && echo "$pci" > /sys/bus/pci/drivers/nvme/unbind
        echo "vfio-pci" > "/sys/bus/pci/devices/$pci/driver_override" 2>/dev/null || true
        [[ -e /sys/bus/pci/drivers/vfio-pci ]] && \
            echo "$pci" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null && \
            echo "  $pci → vfio-pci OK" || true
    done
    VFIO_BOUND=1
    echo "[bind_vfio] 完成"
}

unbind_vfio() {
    [[ "$VFIO_BOUND" == "0" ]] && return 0
    echo "[unbind_vfio] 归还内核 nvme 驱动..."
    for pci in "${DATA_PCI[@]}"; do
        [[ -e "/sys/bus/pci/drivers/vfio-pci/$pci" ]] && \
            echo "$pci" > /sys/bus/pci/drivers/vfio-pci/unbind || true
        echo "" > "/sys/bus/pci/devices/$pci/driver_override" 2>/dev/null || true
        echo "$pci" > /sys/bus/pci/drivers/nvme/bind 2>/dev/null || true
    done
    sleep 2
    mount -a && echo "[unbind_vfio] mount -a OK" || echo "[WARN] mount -a 有错误"
}

trap 'unbind_vfio' EXIT

# hugepages
HP=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
if [[ "$HP" -lt 512 ]]; then
    echo "[error] hugepages 不足 ($HP)，执行: echo 8192 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
    exit 1
fi

# core dump 设置
ulimit -c unlimited
echo "${CORE_DIR}/core.%e.%p" > /proc/sys/kernel/core_pattern
echo "[setup] core dump → ${CORE_DIR}/core.<name>.<pid>"

bind_vfio

echo ""
echo "[run] binary: $BINARY"
echo "[run] sql:    $SQL_FILE"
echo "[run] properties: $PROPERTIES"
echo ""

PIXELS_HOME=/home/whz/opt/pixels/ \
    "$BINARY" /tmp/repro_spdk.db < "$SQL_FILE" 2>&1
EXIT_CODE=$?

echo ""
echo "[done] EXIT_CODE=$EXIT_CODE"
if [[ $EXIT_CODE -ne 0 ]]; then
    echo "[check] core files:"
    ls -lh "${CORE_DIR}"/core.* 2>/dev/null || echo "  (no core dump generated — process exited cleanly, not segfault)"
fi
