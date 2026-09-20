# SPDK test utilities

These host-level utilities support the SPDK performance tests. They target the
mounted Pixels datasets under `/data/9a3-*` and must be used carefully because
VFIO binding temporarily removes those filesystems from the kernel.

Generate the LBA map before unmounting anything:

```bash
python3 testcase/spdk/gen_lba_map.py /data/9a3-{01..24}/clickbench/pixels-e0-fb
```

Bind only the controllers backing the mounted `/data/9a3-*` filesystems:

```bash
sudo testcase/spdk/bind_vfio.sh
```

Always restore the kernel driver and mounts after testing:

```bash
sudo testcase/spdk/unbind_vfio.sh
```

PCI addresses are discovered before unmount and stored in
`/tmp/pixels_spdk_pci_devices`. Set `SPDK_PCI_STATE_FILE` to override that path.
