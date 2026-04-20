# QEMU Size Reduction Strategy

Target: Minimal QEMU for running RISC-V Linux with virtio-9p filesystem and virtio-net.

## Current State

| Build | Size (stripped) |
|-------|-----------------|
| Full build | 15 MB |
| Minimal (virt only) | 5.9 MB |
| TinyEMU reference | 1 MB |

## Analysis: Object File Sizes

### Required (~4MB)
| Size | Component | Notes |
|------|-----------|-------|
| 3.0 MB | target_riscv_translate.c | RISC-V core translation |
| 2.3 MB | target_riscv_vector_helper.c | RISC-V V extension |
| 600 KB | fpu_softfloat.c | Floating point |
| 500 KB | target_riscv_csr.c | RISC-V CSRs |
| 415 KB | cputlb.c | TCG TLB |
| 370 KB | tcg_tcg-op.c | TCG operations |
| 317 KB | system_physmem.c | Physical memory |
| 317 KB | system_vl.c | Main entry |
| 284 KB | hw_virtio_virtio.c | Virtio core |
| 270 KB | tcg_tcg.c | TCG core |
| 268 KB | system_memory.c | Memory subsystem |
| 258 KB | virtio-net.c | Network (needed) |
| 207 KB | hw_9pfs_9p.c | 9p filesystem (needed) |

### Removable - Block Layer (~2.5MB)
| Size | File | Notes |
|------|------|-------|
| 487 KB | block.c | Block core |
| 321 KB | block_qcow2.c | qcow2 format |
| 261 KB | blockdev.c | Block devices |
| 251 KB | nbd_server.c | NBD server |
| 234 KB | block_io.c | Block I/O |
| 230 KB | block-backend.c | Block backend |
| 207 KB | qapi-commands-block-core.c | Block commands |
| 203 KB | qcow2-refcount.c | qcow2 refcount |
| 185 KB | block_nbd.c | NBD client |
| 182 KB | qemu-io-cmds.c | qemu-io |
| 176 KB | qcow2-cluster.c | qcow2 cluster |
| 165 KB | mirror.c | Block mirror |
| 153 KB | file-posix.c | File backend |
| 130 KB | qcow2-bitmap.c | qcow2 bitmap |
| 117 KB | crypto.c | Block crypto |
| 117 KB | block-copy.c | Block copy |
| 112 KB | blkdebug.c | Block debug |

### Removable - QAPI Bloat (~2.2MB)
| Size | File |
|------|------|
| 1028 KB | qapi-introspect.c |
| 572 KB | qapi-visit-block-core.c |
| 175 KB | qapi-visit-machine.c |
| 162 KB | qapi-types-block-core.c |
| 157 KB | qapi-commands-machine.c |
| 137 KB | qapi-visit-qom.c |
| 114 KB | qapi-visit-migration.c |

### Removable - Migration (~800KB)
| Size | File |
|------|------|
| 255 KB | migration_savevm.c |
| 245 KB | migration_ram.c |
| 229 KB | migration_migration.c |
| 126 KB | block-dirty-bitmap.c |

### Removable - SCSI (~450KB)
| Size | File |
|------|------|
| 160 KB | scsi-disk.c |
| 143 KB | scsi-bus.c |
| 131 KB | virtio-scsi.c |

### Removable - Unused Virtio (~450KB)
| Size | File |
|------|------|
| 201 KB | virtio-blk.c |
| 137 KB | virtio-gpu.c |
| 128 KB | virtio-iommu.c |
| 114 KB | virtio-balloon.c |

### Removable - ACPI/SMBIOS (~360KB)
| Size | File |
|------|------|
| 224 KB | aml-build.c |
| 135 KB | smbios.c |

### Removable - PCI (~260KB)
Not needed if using virtio-mmio only.

### Removable - Input/UI/Misc (~530KB)
| Size | File |
|------|------|
| 264 KB | input-keymap.c |
| 116 KB | fw_cfg.c |
| 114 KB | dump_dump.c |

### Removable - Tracing (~480KB)
| Size | File |
|------|------|
| 181 KB | trace-hw_net.c |
| 151 KB | trace-hw_scsi.c |
| 149 KB | trace-migration.c |

## Total Removable: ~8MB

## Implementation Strategy

1. **Device config** (`configs-minimal/riscv64-softmmu/default.mak`):
   - Disable unused machines (spike, sifive_*, etc.)
   - Disable PCI_DEVICES, TEST_DEVICES

2. **Meson options** (requires patching):
   - Disable block drivers except minimal
   - Disable migration
   - Disable SCSI
   - Disable unused virtio devices

3. **Kconfig modifications** (requires patching hw/*/Kconfig):
   - Make ACPI/SMBIOS optional for virt machine
   - Make block layer optional

## Minimal Required Devices for virt + virtio-9p + virtio-net

- RISCV_VIRT machine
- VIRTIO_MMIO
- VIRTIO_9P
- VIRTIO_NET
- VIRTIO_SERIAL (for virtconsole)
- SIFIVE_PLIC (interrupt controller)
- RISCV_ACLINT (timer)
- SERIAL_MM (early console)
