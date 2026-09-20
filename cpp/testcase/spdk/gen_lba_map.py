#!/usr/bin/env python3
"""Generate the file-to-NVMe-LBA map consumed by the SPDK reader.

Run this while the data filesystems are still mounted and owned by the kernel
NVMe driver. VFIO binding makes FIEMAP and mount metadata unavailable.
"""

import argparse
import ctypes
import fcntl
import glob
import json
import os
import tempfile
import re
import subprocess
from pathlib import Path

BLOCK_SIZE = 4096
SECTOR_SIZE = 512
FS_IOC_FIEMAP = 0xC020660B
MAX_EXTENTS = 1024
PCI_BDF_RE = re.compile(r"^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$")


class FiemapExtent(ctypes.Structure):
    _fields_ = [
        ("fe_logical", ctypes.c_uint64),
        ("fe_physical", ctypes.c_uint64),
        ("fe_length", ctypes.c_uint64),
        ("fe_reserved64", ctypes.c_uint64 * 2),
        ("fe_flags", ctypes.c_uint32),
        ("fe_reserved", ctypes.c_uint32 * 3),
    ]


class Fiemap(ctypes.Structure):
    _fields_ = [
        ("fm_start", ctypes.c_uint64),
        ("fm_length", ctypes.c_uint64),
        ("fm_flags", ctypes.c_uint32),
        ("fm_mapped_extents", ctypes.c_uint32),
        ("fm_extent_count", ctypes.c_uint32),
        ("fm_reserved", ctypes.c_uint32),
        ("fm_extents", FiemapExtent * MAX_EXTENTS),
    ]


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True).strip()


def block_device_for(path: str) -> str:
    source = command("findmnt", "-n", "-o", "SOURCE", "--target", path)
    if not source.startswith("/dev/"):
        raise RuntimeError(f"{path}: unsupported mount source {source!r}")
    return os.path.realpath(source)


def namespace_and_partition(path: str) -> tuple[str, str]:
    partition = os.path.basename(block_device_for(path))
    namespace = command("lsblk", "-n", "-o", "PKNAME", f"/dev/{partition}")
    if not namespace:
        namespace = partition
    return namespace, partition


def pci_address(namespace: str) -> str:
    device_path = Path(f"/sys/class/block/{namespace}/device").resolve()
    for part in reversed(device_path.parts):
        if PCI_BDF_RE.match(part):
            return part.lower()
    raise RuntimeError(f"cannot determine PCI address from {device_path}")


def partition_start_lba(partition: str) -> int:
    start_path = Path(f"/sys/class/block/{partition}/start")
    if not start_path.exists():
        return 0
    start_512 = int(start_path.read_text().strip())
    start_bytes = start_512 * SECTOR_SIZE
    if start_bytes % BLOCK_SIZE:
        raise RuntimeError(f"{partition}: partition start is not {BLOCK_SIZE}-byte aligned")
    return start_bytes // BLOCK_SIZE


def fiemap(path: str) -> list[tuple[int, int]]:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        mapping = Fiemap()
        mapping.fm_start = 0
        mapping.fm_length = 0xFFFFFFFFFFFFFFFF
        mapping.fm_flags = 1  # FIEMAP_FLAG_SYNC
        mapping.fm_extent_count = MAX_EXTENTS
        fcntl.ioctl(descriptor, FS_IOC_FIEMAP, mapping)
        if mapping.fm_mapped_extents >= MAX_EXTENTS:
            raise RuntimeError(f"{path}: at least {MAX_EXTENTS} extents; increase MAX_EXTENTS")
        return [
            (mapping.fm_extents[i].fe_physical, mapping.fm_extents[i].fe_length)
            for i in range(mapping.fm_mapped_extents)
        ]
    finally:
        os.close(descriptor)


def scan_file(path: str) -> dict:
    namespace, partition = namespace_and_partition(path)
    partition_offset = partition_start_lba(partition)
    extents = []
    for physical, length in fiemap(path):
        if physical % BLOCK_SIZE:
            raise RuntimeError(f"{path}: FIEMAP extent is not {BLOCK_SIZE}-byte aligned")
        extents.append(
            {
                "lba_start": physical // BLOCK_SIZE + partition_offset,
                "lba_count": (length + BLOCK_SIZE - 1) // BLOCK_SIZE,
            }
        )
    if not extents:
        raise RuntimeError(f"{path}: FIEMAP returned no allocated extents")
    return {
        "pci": pci_address(namespace),
        "file_bytes": os.path.getsize(path),
        "extents": extents,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", help="mounted dataset directories to scan")
    parser.add_argument("--pattern", default="**/*.pxl", help="recursive glob relative to each directory")
    parser.add_argument("--output", default="/tmp/pixels_lba_map.json", help="output JSON path")
    args = parser.parse_args()

    files = []
    for directory in args.directories:
        files.extend(glob.glob(os.path.join(directory, args.pattern), recursive=True))
    files = sorted({os.path.realpath(path) for path in files if os.path.isfile(path)})
    if not files:
        parser.error(f"no files matched pattern {args.pattern!r}")

    result = {}
    for index, path in enumerate(files, 1):
        result[path] = scan_file(path)
        if index % 100 == 0 or index == len(files):
            print(f"[gen_lba_map] scanned {index}/{len(files)}", flush=True)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        # A fixed `${output}.tmp` can be left behind by another user.  This is
        # especially troublesome in sticky directories such as /tmp, where it
        # cannot be overwritten.  Create a private temporary file instead.
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            json.dump(result, temporary, indent=2)
            temporary.write("\n")
        os.replace(temporary_path, output)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    print(f"[gen_lba_map] wrote {len(result)} files to {output}")


if __name__ == "__main__":
    main()
