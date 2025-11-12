# CFGCHK 数据结构设计说明

本文记录 `cfgchk` 校验模块共享数据模型的设计思路。所有字段都直接来源于现有平台配置文件（`board.rs`、zone JSON、zone/root DTS），并说明为何要以当前方式建模。

---

## 1. 设计目标

1. **复用已有事实**：不重新发明配置，而是直接读取 `board.rs`、zone JSON、DTS 的字段。
2. **单次传输**：通过一次 ioctl 把板卡约束与 zone 期望完整送进内核，驱动无需解析文本或访问文件系统。
3. **内核安全易用**：采用定长数组与显式计数，保证 `copy_from_user` 的内存恰好对应，避免动态分配和越界。
4. **跨文件一致性校验**：同时对比板卡定义、JSON 期望、DTS 描述，快速定位冲突（CPU、内存、IRQ、GIC）。

---

## 2. 数据来源

| 数据源 | 关键字段 | 作用 |
| --- | --- | --- |
| `platform/*/board.rs` | `BOARD_NCPUS`、`ROOT_ZONE_CPUS`、`BOARD_MPIDR_MAPPINGS`、`BOARD_PHYSMEM_LIST`、`ROOT_ZONE_IRQS`、`ROOT_ARCH_ZONE_CONFIG` | 描述硬件极限：CPU 拓扑、内存段、IRQ 占用、GIC 参数。 |
| `platform/*/configs/zoneX-*.json` | `zone_id`、`cpus[]`、`memory_regions[]`、`interrupts[]`、`arch_config.*`、`dtb_filepath` | 定义来宾 zone 申请的资源以及覆盖项。 |
| `platform/*/image/dts/zoneX-*.dts` | `cpu@*`、`memory@*`、`virtio_mmio@*` 节点 | 从 DTS 视角描述同一个来宾；用来与 JSON 做一致性对照。 |
| `platform/*/image/dts/zone0.dts` | `reserved-memory`、root `memory@*` | 标记 root zone 需要保留的内存区域。 |

这些文件中的字段会原封不动地填充到 `cfgchk` 结构中，使得每个成员都有明确的出处。

---

## 3. 设计步骤

### 步骤一：抽象通用结构

```
struct cfgchk_mem_region   // { start, size, type, flags }
struct cfgchk_virtio_desc  // { base, size, irq }
```

无论是板卡、zone 还是 DTS，都存在“内存区域”与 “VirtIO 设备” 的描述，统一结构有助于在内核复用相同的遍历逻辑。

### 步骤二：封装板卡约束 (`cfgchk_board_info`)

核心考虑：

- **CPU 范围与归属**：`BOARD_NCPUS` → `total_cpus`；`ROOT_ZONE_CPUS` → `root_cpu_bitmap`；`BOARD_MPIDR_MAPPINGS` → `mpidr_map[]`，用于 DTS MPIDR 到逻辑 CPU 的映射。
- **IRQ 防冲突**：`ROOT_ZONE_IRQS` → `root_irqs[]`。
- **物理内存合法性**：`BOARD_PHYSMEM_LIST` → `physmem[]`；root DTS `reserved-memory` → `reserved_mem[]`，对应 “必须落在保留区” 的场景。
- **GIC 拓扑**：`ROOT_ARCH_ZONE_CONFIG` 给出版本与基地址，内核对比 zone JSON 避免错误。

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

### 步骤三：保留 zone JSON 语义 (`cfgchk_zone_summary`)

- `zone_id`、`cpus[]`、`cpu_bitmap`：既标识 zone，又支持位运算检测冲突。
- `memory_regions[]`：逐条保留 JSON 中的地址、类型；如 `type=virtio`，同步写入 `virtio[]`。
- `interrupts[]`：与 `virtio[]` 匹配 IRQ，同时用于跨 zone 冲突检测。
- `arch_config.*`：复制 GIC 配置，以确保与板卡一致。

### 步骤四：概括 zone/root DTS (`cfgchk_dts_summary`)

- zone DTS：解析 `cpu@*` → CPU ID（结合 `mpidr_map[]`），`memory@*` → RAM，`virtio_mmio@*` → VirtIO 设备。
- root DTS：`reserved-memory` → `reserved_mem[]`（填回 `cfgchk_board_info`），`memory@*` → 校验 root DTS 是否覆盖所有保留段。

### 步骤五：一次性封装 (`cfgchk_request`)

```
+------------------------------------------------+
| cfgchk_request                                 |
|  version              (协议版本)               |
|  zone_count           (包含同目录其他 JSON)    |
|  target_index         (当前校验的 zone 下标)   |
|  flags                (留作扩展)               |
|                                                |
|  board : cfgchk_board_info                     |
|  zones[] : cfgchk_zone_summary                 |
|  dts_zone : cfgchk_dts_summary                 |
|  dts_root : cfgchk_dts_summary                 |
+------------------------------------------------+
```

- `zone_count` 与 `zones[]` 包含同目录可解析的其他 JSON，方便检查跨 zone 资源冲突。
- `version` 确保将来结构升级时能拒绝旧版 CLI。
- `flags` 空置，用于未来控制严格度或返回模式。

---

## 4. 数据流程图

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
 | 用户态解析 (validate.c)                 |
 | 1. parse_board_file()                   |
 | 2. parse_zone_json()                    |
 | 3. parse_zone_dts()                     |
 | 4. parse_root_dts()                     |
 | 5. build_cfg_request()                  |
 +---------------------+-------------------+
                       |
                       v
              cfgchk_request 内存快照
                       |
                       v
        ioctl(fd="/dev/hvisor_cfgchk",
              cmd=HVISOR_CFG_VALIDATE,
              arg=&cfgchk_request)
                       |
                       v
 +---------------------+-------------------+
 | 内核模块 (cfgchk.c)                    |
 | 1. copy_from_user()                     |
 | 2. validate_cpu()                       |
 | 3. validate_memory()                    |
 | 4. validate_irqs()                      |
 | 5. validate_gic()                       |
 +---------------------+-------------------+
                       |
                       v
            打印校验通过/失败日志
```

---

## 5. 校验覆盖范围

| 校验项 | 使用数据 | 目的 |
| --- | --- | --- |
| CPU 拓扑 | `board.total_cpus`、`board.root_cpu_bitmap`、`zone.cpus[]` / `cpu_bitmap`、`dts_zone.cpus[]`、同目录 `zones[].cpu_bitmap` | 检查 CPU 是否越界、是否重复、JSON 与 DTS 是否一致、是否与 root/其他 zone 冲突。 |
| 内存区段 | `board.physmem[]`、`board.reserved_mem[]`、`zone.mem_regions[]`、`dts_zone.mem_regions[]`、`dts_root.mem_regions[]`、同目录 `zones[].mem_regions[]` | 保证 JSON 内存落在板卡合法区间、VirtIO IO 对齐、要求 “reserved” 的 RAM 确实位于保留区、不同 zone 之间无重叠。 |
| IRQ | `board.root_irqs[]`、`zone.irqs[]`、`zone.virtio[]`、同目录 `zones[].irqs[]`、`dts_zone.virtio[]` | 验证 IRQ 无重复或冲突、JSON 与 DTS 中的 VirtIO IRQ 完全一致。 |
| GIC | `board.gic_version`、`board.gicd/gicr`、`zone.gic_version`、`zone.gicd/gicr` | 防止 zone 覆盖错误的 GIC 配置，保证中断控制器参数一致。 |

---

## 6. 设计亮点

1. **可追溯性**：结构字段的值都能映射到具体配置文件的行号，问题定位直观。
2. **内核简洁**：驱动只需遍历结构体，不需要解析字符串或访问文件系统。
3. **易扩展**：新增校验项只需扩充结构体和解析逻辑，`version` 字段可用来做 ABI 演进。
4. **语义统一**：统一的 `cfgchk_mem_region` / `cfgchk_virtio_desc` 让板卡、JSON、DTS 的比较逻辑保持一致。

---

## 7. 后续方向

- 支持输出更丰富的校验结果（例如警告列表），可通过在 `cfgchk_request` 加入返回缓冲区指针实现。
- 利用 `flags` 控制严格/宽松模式，或者标记“仅报告不阻塞”。
- 若需扩展到 PCI、IVC 等资源，可以在现有结构体上新增字段，并同步解析与校验。

---

通过这样的设计，用户态解析一次配置即可生成内核校验所需的全部上下文；内核校验逻辑专注于资源一致性和防冲突，整体流程既可靠又易于维护。***
