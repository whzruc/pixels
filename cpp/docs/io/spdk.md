# SPDK 替换 io_uring 实现方案

## 1. 背景与目标

### 1.1 现状

Pixels 当前使用 io_uring（`DirectUringRandomAccessFile`）进行异步列数据读取。
在 6 × Samsung PM9A3 3.84TB NVMe 场景下实测性能（q45，48 线程）：

| 指标 | 实测值 | 设备规格 | 带宽利用率 |
|------|--------|---------|-----------|
| 单盘均值带宽 | 5.4 GB/s | 6.8 GB/s | **79%** |
| 单盘峰值带宽 | 6.2 GB/s | 6.8 GB/s | 93% |
| 6 盘合计均值 | 31 GB/s | 40.8 GB/s | 77.8% |
| io_uring 每请求延迟 | ~3 μs | — | — |

iostat `%util` 报告 84–94%，但这是时间维度的利用率（设备有请求在处理的时间占比），
对于支持 64K 并行队列的 NVMe 设备，该指标不代表带宽饱和程度。
实际带宽利用率约 79%，存在 ~21% 的提升空间。

### 1.2 实现目标

用 SPDK 用户态 NVMe 驱动替换 io_uring，消除内核 VFS/块层开销：

| 指标 | 当前（io_uring） | 目标（SPDK） |
|------|----------------|------------|
| 单盘均值带宽 | 5.4 GB/s | ≥ 6.3 GB/s（93% of 6.8） |
| 6 盘合计 | 31 GB/s | ≥ 37 GB/s |
| 每请求延迟 | ~3 μs | ~1 μs |
| q45 6ssd wall time | 7.7s（pread 基线） | ≤ 6.5s（预期 -15%） |

### 1.3 前提条件与约束

- **Pixels 文件只读不可变**：写入后从不修改，是 fiemap + raw block 方案的核心前提。
- **静态数据集**：ClickBench 数据在基准测试期间不变更。
- **不修改 pixels-core / pixels-common 已有文件**：新增文件，在 `LocalFS::openRaf` 中添加 SPDK 分支。

---

## 2. 硬件环境

```
服务器 NVMe 设备（24 块，用于数据）：
  Samsung MZQL23T8HCLS (PM9A3 3.84TB)
  顺序读: 6.8 GB/s  随机读 4K: 1,000K IOPS

设备到挂载点映射（完整列表）：
  nvme0n1  (0000:c0:03.1) → /data/9a3-01
  nvme1n1  (0000:c0:03.2) → /data/9a3-02
  nvme2n1  (0000:c0:03.3) → /data/9a3-03
  nvme3n1  (0000:c0:03.4) → /data/9a3-04
  nvme4n1  (0000:80:01.1) → /data/9a3-05
  nvme5n1  (0000:80:01.2) → /data/9a3-06
  nvme6n1  (0000:80:01.3) → /data/9a3-07
  nvme7n1  (0000:80:01.4) → /data/9a3-08
  nvme8n1  (0000:80:03.1) → /data/9a3-09
  nvme9n1  (0000:80:03.2) → /data/9a3-10
  nvme10n1 (0000:80:03.3) → /data/9a3-11
  nvme11n1 (0000:80:03.4) → /data/9a3-12
  nvme12n1 (0000:40:01.1) → /data/9a3-13
  nvme13n1 (0000:40:01.2) → /data/9a3-14
  nvme14n1 (0000:40:01.3) → /data/9a3-15
  nvme15n1 (0000:40:01.4) → /data/9a3-16
  nvme16n1 (0000:40:03.1) → /data/9a3-17
  nvme17n1 (0000:40:03.2) → /data/9a3-18
  nvme18n1 (0000:40:03.3) → /data/9a3-19
  nvme19n1 (0000:40:03.4) → /data/9a3-20
  nvme20n1 (0000:00:01.1) → /data/9a3-21
  nvme21n1 (0000:00:01.2) → /data/9a3-22
  nvme22n1 (0000:00:01.3) → /data/9a3-23
  nvme23n1 (0000:00:01.4) → /data/9a3-24
  nvme24n1 (0000:00:03.4) → /boot/efi  ← 系统盘，不参与

文件系统：XFS（XFS 支持 FS_IOC_FIEMAP）
分区起始扇区：2048（对应 1MB 偏移）
物理块大小：4096 字节
```

---

## 3. 核心原理：fiemap + raw block

### 3.1 为什么 Pixels 适合这条路线

实测 Pixels 文件特性：
- 20 个文件抽样，全部是**单 extent**（连续存储，无碎片）
- 最后修改时间 2026-05-29，此后从未改变
- 单个文件示例：`1768380646_0.pxl` → physical=2,085,892,915,200 字节，单段 94MB

单 extent 意味着映射极简：每个文件只需要 `(pci_addr, lba_start, lba_count)` 三个字段。

### 3.2 fiemap 与 LBA 的关系

XFS 文件系统的磁盘布局：

```
NVMe namespace (nvme0n1):
  LBA 0 ─── LBA 2047: 分区表 (1MB)
  LBA 2048: nvme0n1p1 分区起点 (XFS 根，挂载到 /data/9a3-01)
    ...
    LBA 4,074,009,600: Pixels 文件 1768380646_0.pxl 起点
    LBA 4,074,033,208: Pixels 文件结尾 (+24,208 个 4096字节块 = 94MB)
    ...
```

`ioctl(FS_IOC_FIEMAP)` 返回的 `fe_physical` 是从**分区起点**（nvme0n1p1 起点）算的字节偏移，
不是从设备起点（nvme0n1）算的。SPDK 操作的是设备（nvme0n1），因此需要加上分区偏移：

```
namespace_lba = (fe_physical / 4096) + partition_start_lba
partition_start_lba = 2048 (扇区，512字节) / 8 = 256  (4096字节 LBA)
```

注意：`/sys/block/nvme0n1/nvme0n1p1/start` 返回的是 512 字节扇区数（2048），
换算为 4096 字节 LBA 需要除以 8。

---

## 4. 代码架构

### 4.1 新增文件（不修改已有文件）

```
pixels/cpp/
├── testcase/spdk/
│   ├── gen_lba_map.py          # 离线扫描：file → LBA 映射生成器
│   ├── bind_vfio.sh            # 将数据盘从内核 NVMe 驱动解绑并绑定 VFIO
│   └── unbind_vfio.sh          # 恢复：VFIO → 内核 NVMe 驱动（重新挂载）
├── pixels-common/
│   ├── include/physical/
│   │   └── SpdkBufferPool.h    # hugepage 缓冲区管理（替代 BufferPool）
│   └── include/physical/natives/
│       └── DirectSpdkRandomAccessFile.h
│   └── lib/physical/natives/
│       └── DirectSpdkRandomAccessFile.cpp
└── pixels-common/lib/physical/storage/
    └── LocalFS.cpp             # 修改：添加 SPDK 分支（LocalFS::openRaf）
                                # 这是唯一需要修改的已有文件
```

### 4.2 必须修改的已有文件

`pixels-common/lib/physical/storage/LocalFS.cpp`：在 `openRaf()` 最前面加一个 SPDK 分支：

```cpp
std::shared_ptr<PixelsRandomAccessFile> LocalFS::openRaf(const std::string &path) {
    // ── 新增：SPDK 分支 ──────────────────────────────────────────
    bool useSpdk = false;
    try { useSpdk = ConfigFactory::Instance().boolCheckProperty("localfs.enable.spdk"); }
    catch (...) {}
    if (useSpdk) {
        return std::make_shared<DirectSpdkRandomAccessFile>(path);
    }
    // ── 已有逻辑不变 ────────────────────────────────────────────
    bool useStaticBufferPool = ...
    ...
}
```

新增属性（pixels-cpp.properties）：
```
localfs.enable.spdk=false          # 总开关
localfs.spdk.lba_map=/tmp/pixels_lba_map.json  # 映射文件路径
```

`CMakeLists.txt`：添加 SPDK 库链接（条件编译，不影响无 SPDK 环境）：
```cmake
find_package(SPDK QUIET)
if(SPDK_FOUND)
    add_definitions(-DPIXELS_ENABLE_SPDK)
    target_link_libraries(${EXTENSION_NAME} spdk_nvme spdk_env_dpdk rte_eal)
    target_sources(${EXTENSION_NAME} PRIVATE
        pixels-common/lib/physical/natives/DirectSpdkRandomAccessFile.cpp)
endif()
```

---

## 5. 阶段实现说明

### 阶段 0：环境准备

```bash
# 1. 安装 SPDK 依赖
apt install -y libnuma-dev libpciaccess-dev python3-pyelftools

# 2. 编译 SPDK（关闭不需要的子系统，减少编译时间）
git clone https://github.com/spdk/spdk.git /opt/spdk
cd /opt/spdk && git submodule update --init
./configure --with-rdma=no --with-iscsi-initiator=no \
            --without-isal --with-dpdk=/opt/dpdk
make -j$(nproc)

# 3. 配置 hugepage（每个 SPDK 线程约需 256MB 控制内存 + 数据 buffer）
# 48 线程 × 2 buffer × 120MB = 11.5GB，推荐配置 16GB hugepage
echo 8192 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# 或在 grub 中固化：GRUB_CMDLINE_LINUX="default_hugepagesz=2M hugepages=8192"

# 4. 加载 VFIO 内核模块（不需要 UIO，VFIO 更安全）
modprobe vfio-pci
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode  # 如无 IOMMU
```

### 阶段 1：gen_lba_map.py — 离线 LBA 映射生成

**输入**：数据目录（设备还挂载在内核下）
**输出**：`pixels_lba_map.json`

```python
#!/usr/bin/env python3
"""
testcase/spdk/gen_lba_map.py
在设备仍由内核 NVMe 驱动管理时扫描所有 Pixels 文件，
输出 file_path → (pci_addr, lba_start, lba_count) 映射。
必须在 bind_vfio.sh 之前运行。
"""
import fcntl, ctypes, os, glob, json, subprocess, sys

BLOCK_SIZE   = 4096
SECTOR_SIZE  = 512  # /sys/block/.../start 的单位
FS_IOC_FIEMAP = 0xC020660B

class FiemapExtent(ctypes.Structure):
    _fields_ = [
        ('fe_logical',     ctypes.c_uint64),
        ('fe_physical',    ctypes.c_uint64),  # 从分区起点的字节偏移
        ('fe_length',      ctypes.c_uint64),
        ('fe_reserved64',  ctypes.c_uint64 * 2),
        ('fe_flags',       ctypes.c_uint32),
        ('fe_reserved',    ctypes.c_uint32 * 3),
    ]

class Fiemap(ctypes.Structure):
    _fields_ = [
        ('fm_start',          ctypes.c_uint64),
        ('fm_length',         ctypes.c_uint64),
        ('fm_flags',          ctypes.c_uint32),
        ('fm_mapped_extents', ctypes.c_uint32),
        ('fm_extent_count',   ctypes.c_uint32),
        ('fm_reserved',       ctypes.c_uint32),
        ('fm_extents',        FiemapExtent * 1024),
    ]

# /data/9a3-XX → (pci_addr, nvme_dev, partition_dev)
MOUNT_TO_DEVICE = {
    "/data/9a3-01": ("0000:c0:03.1", "nvme0n1",  "nvme0n1p1"),
    "/data/9a3-02": ("0000:c0:03.2", "nvme1n1",  "nvme1n1p1"),
    "/data/9a3-03": ("0000:c0:03.3", "nvme2n1",  "nvme2n1p1"),
    "/data/9a3-04": ("0000:c0:03.4", "nvme3n1",  "nvme3n1p1"),
    "/data/9a3-05": ("0000:80:01.1", "nvme4n1",  "nvme4n1p1"),
    "/data/9a3-06": ("0000:80:01.2", "nvme5n1",  "nvme5n1p1"),
    "/data/9a3-07": ("0000:80:01.3", "nvme6n1",  "nvme6n1p1"),
    "/data/9a3-08": ("0000:80:01.4", "nvme7n1",  "nvme7n1p1"),
    "/data/9a3-09": ("0000:80:03.1", "nvme8n1",  "nvme8n1p1"),
    "/data/9a3-10": ("0000:80:03.2", "nvme9n1",  "nvme9n1p1"),
    "/data/9a3-11": ("0000:80:03.3", "nvme10n1", "nvme10n1p1"),
    "/data/9a3-12": ("0000:80:03.4", "nvme11n1", "nvme11n1p1"),
    "/data/9a3-13": ("0000:40:01.1", "nvme12n1", "nvme12n1p1"),
    "/data/9a3-14": ("0000:40:01.2", "nvme13n1", "nvme13n1p1"),
    "/data/9a3-15": ("0000:40:01.3", "nvme14n1", "nvme14n1p1"),
    "/data/9a3-16": ("0000:40:01.4", "nvme15n1", "nvme15n1p1"),
    "/data/9a3-17": ("0000:40:03.1", "nvme16n1", "nvme16n1p1"),
    "/data/9a3-18": ("0000:40:03.2", "nvme17n1", "nvme17n1p1"),
    "/data/9a3-19": ("0000:40:03.3", "nvme18n1", "nvme18n1p1"),
    "/data/9a3-20": ("0000:40:03.4", "nvme19n1", "nvme19n1p1"),
    "/data/9a3-21": ("0000:00:01.1", "nvme20n1", "nvme20n1p1"),
    "/data/9a3-22": ("0000:00:01.2", "nvme21n1", "nvme21n1p1"),
    "/data/9a3-23": ("0000:00:01.3", "nvme22n1", "nvme22n1p1"),
    "/data/9a3-24": ("0000:00:01.4", "nvme23n1", "nvme23n1p1"),
}

def get_partition_start_lba(ns_dev, part_dev):
    """返回分区起始的 4096字节 LBA（从设备起点算）"""
    start_512 = int(open(f"/sys/block/{ns_dev}/{part_dev}/start").read().strip())
    return start_512 * SECTOR_SIZE // BLOCK_SIZE  # 2048 * 512 / 4096 = 256

def fiemap_file(path):
    """返回 [(fe_physical_bytes, fe_length_bytes), ...] 从分区起点算"""
    fd = os.open(path, os.O_RDONLY)
    try:
        fm = Fiemap()
        fm.fm_start        = 0
        fm.fm_length       = 0xFFFFFFFFFFFFFFFF
        fm.fm_flags        = 1  # FIEMAP_FLAG_SYNC
        fm.fm_extent_count = 1024
        fcntl.ioctl(fd, FS_IOC_FIEMAP, fm)
        extents = []
        for i in range(fm.fm_mapped_extents):
            e = fm.fm_extents[i]
            extents.append((e.fe_physical, e.fe_length))
        return extents
    finally:
        os.close(fd)

def find_mount(path):
    """找到 path 对应的 /data/9a3-XX 挂载点"""
    for mnt in sorted(MOUNT_TO_DEVICE.keys(), key=len, reverse=True):
        if path.startswith(mnt):
            return mnt
    raise ValueError(f"无法确定 {path} 的挂载点")

def scan_dir(data_dir, pattern="**/*.pxl"):
    result = {}
    files = glob.glob(os.path.join(data_dir, pattern), recursive=True)
    for path in sorted(files):
        mnt  = find_mount(path)
        pci, ns_dev, part_dev = MOUNT_TO_DEVICE[mnt]
        part_lba_offset = get_partition_start_lba(ns_dev, part_dev)
        extents = fiemap_file(path)
        lba_extents = []
        for fe_phys, fe_len in extents:
            lba_start = fe_phys // BLOCK_SIZE + part_lba_offset
            lba_count = (fe_len + BLOCK_SIZE - 1) // BLOCK_SIZE
            lba_extents.append({"lba_start": lba_start, "lba_count": lba_count})
        result[path] = {"pci": pci, "extents": lba_extents}
    return result

if __name__ == "__main__":
    data_dirs = sys.argv[1:] if len(sys.argv) > 1 else ["/data"]
    lba_map = {}
    for d in data_dirs:
        lba_map.update(scan_dir(d))
    out = "/tmp/pixels_lba_map.json"
    with open(out, "w") as f:
        json.dump(lba_map, f, indent=2)
    print(f"[gen_lba_map] {len(lba_map)} 文件 → {out}")
```

用法：
```bash
# 扫描所有 24 块盘的 pixels-e0-fb 数据
python3 testcase/spdk/gen_lba_map.py /data/9a3-{01..24}/clickbench/pixels-e0-fb
# 输出: [gen_lba_map] 15360 文件 → /tmp/pixels_lba_map.json
```

### 阶段 2：bind_vfio.sh / unbind_vfio.sh

```bash
#!/bin/bash
# testcase/spdk/bind_vfio.sh — 将 24 块数据盘从内核解绑，交给 SPDK
# 必须在 gen_lba_map.py 之后、SPDK 进程启动之前运行
# 注意：执行后 /data/9a3-01 ... /data/9a3-24 挂载点将不可用

set -euo pipefail

# 数据盘 PCI 地址（排除系统盘 nvme24n1 / 0000:00:03.4）
DATA_PCI=(
    0000:c0:03.1  0000:c0:03.2  0000:c0:03.3  0000:c0:03.4
    0000:80:01.1  0000:80:01.2  0000:80:01.3  0000:80:01.4
    0000:80:03.1  0000:80:03.2  0000:80:03.3  0000:80:03.4
    0000:40:01.1  0000:40:01.2  0000:40:01.3  0000:40:01.4
    0000:40:03.1  0000:40:03.2  0000:40:03.3  0000:40:03.4
    0000:00:01.1  0000:00:01.2  0000:00:01.3  0000:00:01.4
)

# 卸载所有数据盘挂载点
for i in $(seq -w 1 24); do
    umount /data/9a3-$i 2>/dev/null || true
done

# 加载 VFIO
modprobe vfio-pci
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true

# 解绑内核驱动，绑定 VFIO
for pci in "${DATA_PCI[@]}"; do
    if [ -e "/sys/bus/pci/drivers/nvme/$pci" ]; then
        echo "$pci" > /sys/bus/pci/drivers/nvme/unbind
    fi
    echo "$pci" > /sys/bus/pci/drivers/vfio-pci/bind
    echo "[bind_vfio] $pci → vfio-pci"
done
echo "[bind_vfio] 完成，24 块数据盘已交给 SPDK"
```

```bash
#!/bin/bash
# testcase/spdk/unbind_vfio.sh — 恢复：将设备归还内核，重新挂载文件系统
set -euo pipefail

DATA_PCI=(
    0000:c0:03.1  0000:c0:03.2  0000:c0:03.3  0000:c0:03.4
    0000:80:01.1  0000:80:01.2  0000:80:01.3  0000:80:01.4
    0000:80:03.1  0000:80:03.2  0000:80:03.3  0000:80:03.4
    0000:40:01.1  0000:40:01.2  0000:40:01.3  0000:40:01.4
    0000:40:03.1  0000:40:03.2  0000:40:03.3  0000:40:03.4
    0000:00:01.1  0000:00:01.2  0000:00:01.3  0000:00:01.4
)

for pci in "${DATA_PCI[@]}"; do
    if [ -e "/sys/bus/pci/drivers/vfio-pci/$pci" ]; then
        echo "$pci" > /sys/bus/pci/drivers/vfio-pci/unbind
    fi
    echo "$pci" > /sys/bus/pci/drivers/nvme/bind
    echo "[unbind_vfio] $pci → nvme"
done

# 等待内核枚举设备
sleep 2

# 重新挂载（依赖 /etc/fstab）
mount -a
echo "[unbind_vfio] 完成，挂载点已恢复"
```

### 阶段 3：DirectSpdkRandomAccessFile

接口设计与 `DirectUringRandomAccessFile` 保持一致，让 `LocalFS` 可以透明切换。

**pixels-common/include/physical/natives/DirectSpdkRandomAccessFile.h**：

```cpp
#pragma once
#ifdef PIXELS_ENABLE_SPDK

#include "physical/natives/PixelsRandomAccessFile.h"
#include "spdk/nvme.h"
#include <atomic>
#include <string>

// LBA 映射条目（来自 pixels_lba_map.json 中单个 extent 的记录）
struct SpdkLbaExtent {
    uint64_t lba_start;
    uint32_t lba_count;
};

// 在主线程初始化一次，之后只读
class SpdkContext {
public:
    // 调用一次：probe 所有 VFIO 设备，加载 LBA 映射
    static void Initialize(const std::string& lba_map_path);
    static void Shutdown();

    // 根据文件路径查找 (controller, namespace, extents)
    static bool Lookup(const std::string& path,
                       spdk_nvme_ctrlr** ctrlr_out,
                       spdk_nvme_ns**    ns_out,
                       std::vector<SpdkLbaExtent>& extents_out);
private:
    struct DeviceEntry {
        spdk_nvme_ctrlr* ctrlr;
        spdk_nvme_ns*    ns;
        std::string      pci_addr;
    };
    static std::unordered_map<std::string, DeviceEntry> pci_to_device;
    static std::unordered_map<std::string, std::pair<std::string, std::vector<SpdkLbaExtent>>> file_map;
};

class DirectSpdkRandomAccessFile : public PixelsRandomAccessFile {
public:
    explicit DirectSpdkRandomAccessFile(const std::string& path);
    ~DirectSpdkRandomAccessFile() override;

    // 线程本地初始化（每个 DuckDB worker 线程调用一次）
    static void InitThread();   // 创建 thread-local qpair
    static void ResetThread();  // 释放 thread-local qpair

    // PixelsRandomAccessFile 接口
    long length() override;
    void seek(long off) override;
    std::shared_ptr<ByteBuffer> readFully(int len) override;
    std::shared_ptr<ByteBuffer> readFully(int len, std::shared_ptr<ByteBuffer> bb) override;
    long readLong() override;
    int  readInt() override;
    char readChar() override;
    void close() override;
    std::string getName() override;
    std::string getPath() override;
    bool supportsAsync() override { return true; }

    // 异步接口（与 DirectUringRandomAccessFile 相同语义）
    std::shared_ptr<ByteBuffer> readAsync(int length,
                                          std::shared_ptr<ByteBuffer> buffer,
                                          int index);
    void readAsyncSubmit(int size);    // SPDK 无需显式 submit，此为空操作
    void readAsyncComplete(int size);  // 轮询 qpair 直到所有完成

private:
    std::string   file_path;
    long          file_len;
    long          offset = 0;
    spdk_nvme_ctrlr* ctrlr;
    spdk_nvme_ns*    ns;
    std::vector<SpdkLbaExtent> extents;

    // Thread-local qpair（类比 io_uring ring）
    static thread_local spdk_nvme_qpair* qpair;
    static thread_local int pending_count;
};

#endif // PIXELS_ENABLE_SPDK
```

**关键实现逻辑（pixels-common/lib/physical/natives/DirectSpdkRandomAccessFile.cpp）**：

```cpp
// readAsync：将列读取请求提交到 NVMe SQ（非阻塞，立即返回）
std::shared_ptr<ByteBuffer>
DirectSpdkRandomAccessFile::readAsync(int length,
                                       std::shared_ptr<ByteBuffer> buffer,
                                       int index)
{
    // 计算文件内字节偏移 → namespace LBA
    // 当前 Pixels 文件均为单 extent，直接计算
    uint64_t byte_offset = offset;
    uint64_t ns_lba      = extents[0].lba_start + byte_offset / BLOCK_SIZE;
    uint32_t n_blocks    = (length + BLOCK_SIZE - 1) / BLOCK_SIZE;

    struct ReadContext { std::atomic<bool> done{false}; int result{0}; };
    auto* ctx = new ReadContext();

    spdk_nvme_ns_cmd_read(
        ns, qpair,
        buffer->getPointer(),     // 必须是 spdk_dma_malloc 分配的内存
        ns_lba, n_blocks,
        [](void* arg, const struct spdk_nvme_cpl* cpl) {
            auto* c = static_cast<ReadContext*>(arg);
            c->result = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;
            c->done.store(true, std::memory_order_release);
        },
        ctx, 0);

    pending_count++;
    seek(offset + length);
    return std::make_shared<ByteBuffer>(*buffer, 0, length);
}

// readAsyncSubmit：SPDK cmd_read 无需显式 submit，此为空操作
void DirectSpdkRandomAccessFile::readAsyncSubmit(int size) { /* no-op */ }

// readAsyncComplete：轮询 CQ 直到所有请求完成
void DirectSpdkRandomAccessFile::readAsyncComplete(int size)
{
    // 与 double-buffer 完全兼容：此函数在解码下一个 buffer 之前被调用，
    // CPU 已做完上一轮解码，此时轮询 CQ 代价很低
    while (pending_count > 0) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
    pending_count = 0;
}
```

### 阶段 4：SpdkBufferPool — hugepage 缓冲区管理

SPDK DMA 读取要求目标 buffer 是 hugepage 内存（IOMMU 锁定），普通 `posix_memalign` 分配的内存不能作为 DMA 目标。

```cpp
// pixels-common/include/physical/SpdkBufferPool.h
class SpdkBufferPool {
public:
    // 替代 BufferPool::Initialize()，分配 spdk_dma_malloc 内存
    static void Initialize(const std::vector<uint32_t>& colIds, int n_col_groups);
    static void Reset();

    // buffers[group][col_idx] — 与 BufferPool::buffers 接口兼容
    static std::vector<std::vector<std::shared_ptr<ByteBuffer>>> buffers;

private:
    // 底层内存由 spdk_dma_malloc 分配，通过 custom deleter 归还
    static std::vector<void*> raw_allocations;
};
```

`BufferPool` 和 `SpdkBufferPool` 共存，`LocalFS::openRaf` 选择后，
对应的初始化函数（`PixelsScanFunction::InitLocal` 中）也需要切换。

---

## 6. 双 buffer 兼容性

双 buffer 流水线与 SPDK 轮询模型完全兼容，时序对比如下：

```
io_uring 双 buffer：
  ┌─────────────────────────┬──────────────────────────┐
  │ submit SQE (file N+1)   │ wait CQE (file N+1)      │
  ├─────────────────────────┤                           │
  │        decode file N                               │
  └──────────────────────────────────────────────────── ┘

SPDK 双 buffer（等价）：
  ┌──────────────────────────┬─────────────────────────┐
  │ cmd_read (file N+1)      │ process_completions      │
  │ [非阻塞，立即返回]        │ [轮询直到 done]          │
  ├──────────────────────────┤                          │
  │         decode file N                              │
  └─────────────────────────────────────────────────── ┘
```

SPDK 相比 io_uring 的优势：`process_completions` 是纯 CPU 轮询（no syscall），
不会有内核态切换的开销，在解码结束后等待 I/O 完成时更轻量。

---

## 7. 测试计划

### 7.1 LBA 映射正确性测试

**目的**：验证 `gen_lba_map.py` 生成的映射和 partition offset 计算完全正确。

测试方法：在设备仍由内核管理时，对比 `pread()` 读取 vs 通过 LBA 映射用 `/dev/nvme0n1`
直接读取（两者结果应完全一致）：

```bash
# 测试脚本：testcase/spdk/test_lba_correctness.py
python3 - <<'PY'
import json, os

lba_map = json.load(open("/tmp/pixels_lba_map.json"))
BLOCK_SIZE = 4096

test_files = list(lba_map.items())[:10]  # 抽取前 10 个文件

for path, info in test_files:
    extents = info["extents"]
    assert len(extents) == 1, f"{path} 有 {len(extents)} 个 extent，预期 1"
    lba_start = extents[0]["lba_start"]
    lba_count = extents[0]["lba_count"]

    # 找到对应的 nvme 设备
    pci = info["pci"]
    # 通过 pci 找到 nvme 设备名（查 /sys/bus/pci/devices/{pci}/nvme/）
    nvme_dev = os.listdir(f"/sys/bus/pci/devices/{pci}/nvme/")[0]  # 例如 nvme0
    ns_dev = f"/dev/{nvme_dev}n1"  # 例如 /dev/nvme0n1

    # 方法1：pread 读文件（通过内核/VFS）
    with open(path, "rb") as f:
        expected = f.read(BLOCK_SIZE * 4)

    # 方法2：直接 LBA 读取（绕过文件系统）
    with open(ns_dev, "rb") as f:
        f.seek(lba_start * BLOCK_SIZE)
        actual = f.read(BLOCK_SIZE * 4)

    assert expected == actual, f"数据不一致: {path}"
    print(f"  ✓ {os.path.basename(path)}: LBA {lba_start}, {lba_count} blocks")

print(f"\n{len(test_files)} 个文件 LBA 映射验证通过")
PY
```

**通过标准**：所有抽样文件读取内容与 pread 完全一致（逐字节匹配）。

### 7.2 SPDK 设备初始化测试

**目的**：验证 SPDK probe、qpair 分配、基本读取全流程正常。

```bash
# 运行前：bind_vfio.sh 已执行，hugepage 已配置
# 测试程序：testcase/spdk/test_spdk_init
./build/test_spdk_init \
    --lba-map /tmp/pixels_lba_map.json \
    --test-file /data/9a3-01/clickbench/pixels-e0-fb/1768380646_0.pxl \
    --verify-against-pread  # 需要在 unbind 前预先读取参考数据
```

测试内容：
- [ ] SPDK 成功 probe 24 个 NVMe 控制器
- [ ] 每个控制器可分配 io_qpair
- [ ] 读取测试文件头部 4096 字节，与参考数据匹配
- [ ] 读取测试文件中间某列偏移，与参考数据匹配
- [ ] 48 个并发线程各自读取，无数据竞争

### 7.3 DuckDB 集成功能测试

**目的**：通过 DuckDB 层验证 SPDK 读取的数据语义正确性。

测试步骤：

```bash
# 步骤 1（内核模式）：记录参考结果
./duckdb.bin -c "
  LOAD '/path/to/pixels.duckdb_extension';
  SET pixels_enable_spdk=false;
  SELECT count(*) FROM pixels_scan(['/data/9a3-01/clickbench/pixels-e0-fb/*']);
" > /tmp/ref_count.txt

./duckdb.bin -c "
  LOAD '/path/to/pixels.duckdb_extension';
  SET pixels_enable_spdk=false;
  SELECT watchid, eventtime FROM pixels_scan(['/data/9a3-01/.../*'])
  WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10;
" > /tmp/ref_q45.txt

# 步骤 2：切换 SPDK 模式
sudo bash testcase/spdk/bind_vfio.sh

# 步骤 3（SPDK 模式）：运行相同查询
./duckdb.bin -c "
  LOAD '/path/to/pixels.duckdb_extension';
  SET pixels_enable_spdk=true;
  SELECT count(*) FROM pixels_scan(['/data/9a3-01/clickbench/pixels-e0-fb/*']);
" > /tmp/spdk_count.txt

./duckdb.bin -c "
  LOAD '/path/to/pixels.duckdb_extension';
  SET pixels_enable_spdk=true;
  SELECT watchid, eventtime FROM pixels_scan(['/data/9a3-01/.../*'])
  WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10;
" > /tmp/spdk_q45.txt

# 步骤 4：比对
diff /tmp/ref_count.txt /tmp/spdk_count.txt  # 预期无差异
diff /tmp/ref_q45.txt   /tmp/spdk_q45.txt    # 预期无差异
```

**通过标准**：
- [ ] `count(*)` 返回 99,997,497（单盘 1 份）
- [ ] q45 返回完全相同的 10 行（含顺序）

### 7.4 性能基准测试

**目的**：量化 SPDK vs io_uring 的带宽和延迟提升。

集成到现有 benchmark suite，新增 `run_suite_spdk_pixels.sh`：

```bash
# 运行方式（需已执行 bind_vfio.sh）
QUERIES=q45 \
BENCHMARK_PREFIX=clickbench-spdk-pixels-e0 \
SSD_MODES="1ssd 6ssd 24ssd" \
BUFFER_MODES="spdk-singlebuffer spdk-doublebuffer" \
THREAD_LIST="1 8 24 48" \
RUN_PERF_STAT=1 RUN_IOSTAT=1 RUN_PERF_ONCPU=1 \
SUITE_TAG="spdk-pixels-$(date +%Y%m%d_%H%M%S)" \
sudo -E ./run_suite_spdk_pixels.sh
```

新增 `lib_suite_common.sh` 中的 buffer 模式：

```python
"spdk-singlebuffer": {
    "localfs.enable.spdk": "true",
    "pixels.doublebuffer": "false",
    "localfs.enable.async.io": "false",
    "pixel.enable.globalStaticBytebuffer": "false",
},
"spdk-doublebuffer": {
    "localfs.enable.spdk": "true",
    "pixels.doublebuffer": "true",
    "localfs.enable.async.io": "false",
    "pixel.enable.globalStaticBytebuffer": "false",
},
```

**性能目标（通过标准）**：

| 对比维度 | 基线（io_uring） | SPDK 目标 | 最低接受线 |
|---------|----------------|----------|-----------|
| 单盘均值带宽 | 5.4 GB/s | ≥ 6.3 GB/s | ≥ 5.8 GB/s |
| 6盘 q45 wall time | 7.7s | ≤ 6.5s | ≤ 7.0s |
| 24盘 q45 wall time | 待测 | 较 io_uring 降低 ≥ 10% | 降低 ≥ 5% |
| 每请求延迟（iostat mean_r_await_ms） | 0.85ms | ≤ 0.4ms | ≤ 0.6ms |

### 7.5 稳定性测试

**目的**：验证长时间运行不出现内存泄漏、双 free、qpair 竞争等问题。

```bash
# 连续运行 30 次 q45，检查结果一致性和内存占用
for i in $(seq 1 30); do
    ./duckdb.bin -c "
      SET pixels_enable_spdk=true;
      SELECT count(*) FROM pixels_scan([...]);
    "
    echo "第 $i 次: $(date)"
done

# 检查 hugepage 内存是否有泄漏（每次运行后应回到基线）
grep HugePages_Free /proc/meminfo
```

**通过标准**：
- [ ] 30 次运行结果全部返回 99,997,497
- [ ] 每次运行结束后 hugepage 空闲量恢复到初始值（无泄漏）
- [ ] 无 segfault，无 SPDK assertion failure

### 7.6 回归测试

确保 SPDK 模式不影响 io_uring 模式的正确性：

```bash
# 恢复内核驱动
sudo bash testcase/spdk/unbind_vfio.sh

# 运行原有完整 benchmark suite（io_uring 模式）
QUERIES=q45 SSD_MODES="6ssd" BUFFER_MODES="singlebuffer doublebuffer" \
SUITE_TAG="regression-after-spdk" sudo -E ./run_suite.sh
```

**通过标准**：io_uring 模式性能与 SPDK 实现合入前完全一致（误差 ±3%）。

### 7.7 阶段耗时分析

在 `pixels-cpp.properties` 中启用：

```properties
pixel.enable.profiler=true
```

查询结束后，现有 Pixels profiler 输出之后会增加 `SPDK I/O Profile Summary`，其中包括：

- `Spdk.Initialize.*`：DPDK/SPDK 环境初始化、NVMe probe 和 LBA map 加载。
- `Spdk.BufferPool.Initialize.*`：DMA Buffer 初次分配和扩容。
- `Spdk.SyncRead.*`：文件头尾同步读取中的 DMA 分配、提交、轮询、复制和释放。
- `Spdk.AsyncRead.Submit`：异步 NVMe 命令构造与提交。
- `Spdk.AsyncComplete.*`：completion polling 和请求描述符释放。

`thread_time_s` 是所有扫描线程的累计时间。SPDK 使用忙轮询，因此 `Spdk.AsyncComplete.Poll` 同时代表 I/O 等待和消耗掉的用户态 CPU 时间。

---

## 8. 实现工作量估算

| 阶段 | 内容 | 工作量 |
|------|------|--------|
| 阶段 0 | SPDK 编译、VFIO 配置、hugepage 设置 | 3 天 |
| 阶段 1 | `gen_lba_map.py` + LBA 正确性验证 | 4 天 |
| 阶段 2 | `bind_vfio.sh` / `unbind_vfio.sh` | 2 天 |
| 阶段 3 | `DirectSpdkRandomAccessFile`（含 SpdkContext） | 1 周 |
| 阶段 4 | `SpdkBufferPool`（hugepage 分配） | 3 天 |
| 阶段 5 | `LocalFS::openRaf` 新增 SPDK 分支 | 1 天 |
| 阶段 6 | 集成测试 + benchmark suite 脚本 | 4 天 |
| 阶段 7 | 调试、稳定性验证 | 1 周 |
| **合计** | | **约 4 周** |

---

## 9. 风险与注意事项

1. **系统盘保护**：`nvme24n1`（0000:00:03.4，挂载 `/boot/efi` 和 `/`）绝对不能绑定 VFIO。
   脚本中已硬编码排除，执行前请再次确认 PCI 地址。

2. **断电恢复**：VFIO 绑定不持久化，重启后设备自动恢复内核驱动。
   如需持久化 VFIO 绑定，在 `/etc/modprobe.d/` 和 `/etc/udev/rules.d/` 中配置。

3. **多进程冲突**：同一设备只能由一个 SPDK 进程独占。
   运行 Pixels 时确保没有其他进程（如 nvme-cli）访问同一设备。

4. **fiemap 时机**：`gen_lba_map.py` 必须在数据写入完成且文件系统 flush 之后、
   `bind_vfio.sh` 之前运行。正确顺序：写数据 → sync → gen_lba_map.py → bind_vfio.sh。

5. **XFS 碎片**：XFS 长期使用后可能出现碎片，导致新写入文件有多个 extent。
   现有数据（单 extent 100%）不受影响，新增数据写入后应验证 extent 数量再更新映射。
