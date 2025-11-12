# CFGCHK Data Structure Design Notes

This document captures the reasoning process behind the shared data model used by the `cfgchk` validation module. Everything below is derived directly from existing platform assets (`board.rs`, zone JSON, zone/root DTS) and explains why each structure was shaped the way it is.

---

## 1. Objectives

1. **Single source of truth** – reuse the configuration data already present in `board.rs`, zone JSON files, and DTS bundles instead of inventing new metadata.
2. **One-shot transfer** – deliver a complete snapshot of board and zone intent to the kernel through a single ioctl so the driver never parses text or touches the filesystem.
3. **Safe to consume in kernel** – use fixed-size arrays and explicit element counts so that `copy_from_user` memory fits exactly, avoids dynamic allocation, and fails fast on malformed payloads.
4. **Cross-file consistency check** – compare “what the board says”, “what the zone JSON requests”, and “what the DTS advertises” to surface conflicts (CPU, memory, IRQ, GIC).

---

## 2. Inputs & Evidence

The first step was to catalogue the configuration sources and the fields they already expose.

| Input file | Relevant fields | Why they matter |
| --- | --- | --- |
| `platform/*/board.rs` | `BOARD_NCPUS`, `ROOT_ZONE_CPUS`, `BOARD_MPIDR_MAPPINGS`, `BOARD_PHYSMEM_LIST`, `ROOT_ZONE_IRQS`, `ROOT_ARCH_ZONE_CONFIG` | Defines hardware limits (CPU topology, memory map, IRQ ownership, GIC layout). |
| `platform/*/configs/zoneX-*.json` | `zone_id`, `cpus[]`, `memory_regions[]`, `interrupts[]`, `arch_config.*`, `dtb_filepath` | Declares the guest’s requested resources and overrides. |
| `platform/*/image/dts/zoneX-*.dts` | `cpu@*` nodes, `memory@*` nodes, `virtio_mmio@*` nodes | Describes the same guest from the DTS perspective; used for cross-checking JSON. |
| `platform/*/image/dts/zone0.dts` | `reserved-memory` nodes, root `memory@*` | Provides the regions that must stay reserved for the root zone. |

These references make the structures tangible—every field we keep has a direct piece of configuration that fills it.

---

## 3. Design Walkthrough

### Step 1 – Normalise recurring shapes

Before summarising board or zone data, we defined two reusable blocks:

```
struct cfgchk_mem_region   // { start, size, type, flags }
struct cfgchk_virtio_desc  // { base, size, irq }
```

They appear in all three summaries (board, zone, DTS). Keeping one canonical format means a single comparison loop can reason about RAM vs IO vs VirtIO segments everywhere.

### Step 2 – Capture board-level constraints (`cfgchk_board_info`)

Key considerations:

- **CPU scope & ownership**: `BOARD_NCPUS` sets `total_cpus`; `ROOT_ZONE_CPUS` becomes a bitmap for O(1) collision checks; `BOARD_MPIDR_MAPPINGS` lets us translate DTS MPIDR values into logical CPU indices.
- **Interrupt reservations**: `ROOT_ZONE_IRQS` becomes an array so we can block zone IRQs from reusing root numbers.
- **Memory viability**: `BOARD_PHYSMEM_LIST` enumerates allowed [start, end, type]; we store them verbatim to ensure JSON RAM falls entirely inside a legal segment. `reserved_mem[]` inherits root DTS `reserved-memory` to enforce “must live in reserved” semantics.
- **GIC topology**: `ROOT_ARCH_ZONE_CONFIG` fixes GIC version/bases; matching these numbers catches misconfigured JSON/DTS quickly.

```
+------------------------+
| cfgchk_board_info      |
|  total_cpus            | <- BOARD_NCPUS
|  root_cpu_bitmap       | <- ROOT_ZONE_CPUS
|  mpidr_map[]           | <- BOARD_MPIDR_MAPPINGS
|  root_irqs[]           | <- ROOT_ZONE_IRQS
|  physmem[]             | <- BOARD_PHYSMEM_LIST
|  reserved_mem[]        | <- zone0.dts reserved-memory
|  gic_version / bases   | <- ROOT_ARCH_ZONE_CONFIG
+------------------------+
```

### Step 3 – Preserve zone JSON intent (`cfgchk_zone_summary`)

From the guest configuration:

- `zone_id`, `cpus[]`, `cpu_bitmap`, `cpu_count` allow both identity and fast bitmask conflict detection.
- `memory_regions[]` reflect each JSON entry. When `type` is `virtio`, we populate the sibling `cfgchk_virtio_desc` record, because the kernel needs base/size/irq for integrity checks.
- `interrupts[]` connect VirtIO devices to actual IRQ numbers and help catch duplicates across zones.
- `arch_config.*` fields copy over to enforce GIC consistency with the board summary.

### Step 4 – Outline DTS expectations (`cfgchk_dts_summary`)

For both the target zone and the root zone we keep:

- The list of CPU IDs parsed from `cpu@` nodes (after MPIDR conversion).
- RAM nodes from `memory@*`.
- VirtIO nodes (base/size/irq) for parity with JSON.

Root DTS additionally feeds `reserved_mem[]` in the board summary; however, the root DTS snapshot itself (in `dts_root`) lets the kernel assert that root reserved-memory covers every reserved range we advertised to the JSON.

### Step 5 – Ship everything in one parcel (`cfgchk_request`)

```
+------------------------------------------------+
| cfgchk_request                                 |
|  version              (ABI guard)              |
|  zone_count           (peer zones included)    |
|  target_index         (which zone we focus on) |
|  flags                (future extensibility)   |
|                                                |
|  board : cfgchk_board_info                     |
|  zones[] : cfgchk_zone_summary                 |
|  dts_zone : cfgchk_dts_summary                 |
|  dts_root : cfgchk_dts_summary                 |
+------------------------------------------------+
```

- `zone_count` + `zones[]` include peer JSON files from the same directory so the kernel can detect cross-zone conflicts without additional syscalls.
- A `version` field allows bumping the structure safely if new columns are added later.
- The `flags` slot purposely stays unused today—reserved for toggling strict vs tolerant behaviour or optional warnings.

---

## 4. Data Flow Illustration

```
           +-------------------------+
           | board.rs                |
           | - BOARD_NCPUS           |
           | - ROOT_ZONE_CPUS        |
           | - BOARD_PHYSMEM_LIST    |
           | - ...                   |
           +-----------+-------------+
                       |
                       v
 +---------------------+-------------------+
 | User-space parser (validate.c)          |
 | 1. parse_board_file()                   |
 | 2. parse_zone_json()                    |
 | 3. parse_zone_dts()                     |
 | 4. parse_root_dts()                     |
 | 5. build_cfg_request()                  |
 +---------------------+-------------------+
                       |
                       v
              cfgchk_request (in RAM)
                       |
                       v
        ioctl(fd="/dev/hvisor_cfgchk",
              cmd=HVISOR_CFG_VALIDATE,
              arg=&cfgchk_request)
                       |
                       v
 +---------------------+-------------------+
 | Kernel module (cfgchk.c)                |
 | 1. copy_from_user()                     |
 | 2. validate_cpu()                       |
 | 3. validate_memory()                    |
 | 4. validate_irqs()                      |
 | 5. validate_gic()                       |
 +---------------------+-------------------+
                       |
                       v
            Pass / Fail decision logged
```

The flow shows how each parsing step contributes to the final request, and how the kernel consumes it without ever seeing the original text files.

---

## 5. Validation Coverage

| Check | Data consumed | Purpose |
| --- | --- | --- |
| CPU topology | `board.total_cpus`, `board.root_cpu_bitmap`, `zone.cpus`, `dts_zone.cpus`, peer `zones[].cpu_bitmap` | Ensure CPU IDs exist, avoid duplication, enforce JSON ↔ DTS match, prevent overlap with root/other zones. |
| Memory ranges | `board.physmem`, `board.reserved_mem`, `zone.mem_regions`, `dts_zone.mem_regions`, `dts_root.mem_regions`, peer zones | Guarantee JSON RAM fits board ranges, virtio IO segments align, reserved-only regions stay in reserved-memory, detect cross-zone overlap. |
| IRQ ownership | `board.root_irqs`, `zone.irqs`, `zone.virtio`, peer zones, `dts_zone.virtio` | Catch duplicate or conflicting IRQs and enforce JSON ↔ DTS parity for VirtIO IRQ binding. |
| GIC configuration | `board.gic_version`, base/size; `zone.gic_version`, base/size | Detect mismatches that would break interrupt delivery. |

Each of these checks would be impossible (or very expensive) if the data structures did not mirror the real configuration files.

---

## 6. Why this approach feels “solid”

1. **Traceability** – every field references a concrete line in repository configuration, so problems can be traced back instantly.
2. **Kernel simplicity** – the driver performs bounded loops over structs; no string parsing, no filesystem access, no allocations beyond a single `kmalloc`.
3. **Extensibility** – adding a new validation rule is a matter of extending the structs plus the parsing and check routines; ioctl shape stays consistent via `version`.
4. **Consistent semantics** – the same struct layout is used for board, zone JSON, and DTS data, allowing re-use of helper logic (`range_within`, `reserved_contains`, etc.).

---

## 7. Next Steps

- Add optional reporting output (e.g., a warning list) by adding an output buffer pointer to `cfgchk_request`.
- Introduce stricter flag-based policies (for example, a “warning mode” that collects issues but still allows boot).
- Extend summaries with PCI or IVC metadata once those need kernel-level validation.

These enhancements can build on the existing structure without rewriting the communication path.

---

This design keeps the kernel and user space in lockstep while staying grounded in the actual configuration artifacts shipped with each platform. Whenever a board evolves, the structures automatically absorb the new truth as long as the parsing logic recognises it, making the validation flow both reliable and maintainable.
