# hvisor 配置校验实现细节

本文深入解析 `hvisor-tool` 中“配置校验”功能的实现，逐阶段梳理用户态与内核态协同逻辑，并对关键代码片段逐行解释。涉及的核心文件如下：

| 角色 | 文件 | 主要函数 |
| --- | --- | --- |
| 用户态 CLI | `tools/hvisor.c` | `main` |
| 校验命令实现 | `tools/validate.c` | `zone_validate_command` 及配套解析函数 |
| 共享头文件 | `include/cfgchk.h` | 公共结构体/常量 |
| 内核模块 | `driver/cfgchk.c` | `cfgchk_validate_request`、`validate_*` |

下文按照执行流程拆解每个阶段。

---

## 1. CLI 入口：`tools/hvisor.c`

### 1.1 命令分派

```c
// tools/hvisor.c:689-706
if (strcmp(argv[1], "zone") == 0) {
    ...
    } else if (strcmp(argv[2], "validate") == 0) {
        err = zone_validate_command(argc - 3, &argv[3]);
    } else {
        help(1);
    }
}
```

- `strcmp(argv[2], "validate")`：判断子命令是否为 `validate`。
- `zone_validate_command(argc - 3, &argv[3])`：把 `zone validate <config.json>` 之后的参数交给校验实现。

---

## 2. 用户态解析阶段（`tools/validate.c`）

### 2.1 总体流程：`zone_validate_command`

```c
// tools/validate.c:771-817（节选）
if (!realpath(argv[0], abs_json)) { ... }
dir_path = dirname(json_copy);
...
if (resolve_config_path(board_path, ...) != 0) return -1;
if (resolve_config_path(zone_dts_path, ...) != 0) return -1;
if (resolve_config_path(root_dts_path, ...) != 0) return -1;

if (parse_board_file(board_path, &board) != 0) return -1;
if (parse_zone_json(abs_json, &target_zone, NULL, 0) != 0) return -1;
if (parse_zone_dts(zone_dts_path, &dts_zone, &board) != 0) return -1;
if (parse_root_dts(root_dts_path, &dts_root, &board) != 0) return -1;

apply_reservation_flags(&board, &target_zone);
if (build_cfg_request(abs_json, &req, &target_zone, &dts_zone, &dts_root, &board) != 0) return -1;

fd = open("/dev/hvisor_cfgchk", O_RDWR);
ret = ioctl(fd, HVISOR_CFG_VALIDATE, &req);
```

- `realpath(argv[0], abs_json)`：获取 JSON 绝对路径；失败直接退出。
- `resolve_config_path(...)`：按约定目录查找 `board.rs`、zone/root DTS，并提供回退路径。
- `parse_*` 函数：把文件解析到结构体（详见下节）。
- `apply_reservation_flags()`：标记需要 reserved-memory 的内存段。
- `build_cfg_request()`：拼装 `cfgchk_request` 并收集其它 zone 的 JSON。
- `open + ioctl`：把请求发送给内核模块。

### 2.2 解析 `board.rs`：`parse_board_file`

#### 2.2.1 基础结构与 NCPU

```c
cursor = strstr(buf, "BOARD_NCPUS");
...
if (!expr_start || parse_first_unsigned(expr_start + 1, &cpus) != 0) {
    unsigned int derived;
    if (derive_cpus_from_mpidr(buf, &derived) != 0) { ... }
    log_warn("BOARD_NCPUS 表达式无法解析...");
    board->total_cpus = derived;
} else {
    board->total_cpus = (unsigned int)cpus;
}
```

- `parse_first_unsigned`：从 `BOARD_NCPUS` 行提取第一个数字。
- 若失败（宏表达式或缺失），退回 `BOARD_MPIDR_MAPPINGS` 的长度推导，并提示 WARN。

#### 2.2.2 根区 CPU 位图

```c
cursor = strstr(buf, "ROOT_ZONE_CPUS");
...
char *token = strtok(expr_buf, "|");
while (token) {
    token = trim_whitespace(token);
    if (sscanf(token, "(1 << %u)", &shift) == 1) {
        board->root_cpu_bitmap |= 1ULL << shift;
    } else if (parse_hex_or_dec(token, &val) == 0 && val < 64) {
        board->root_cpu_bitmap |= 1ULL << val;
    }
    token = strtok(NULL, "|");
}
```

- 将位运算表达式 `(1 << n)` 或直接数字解析成 CPU 位图，用于后续与 guest CPU 冲突检测。

#### 2.2.3 物理内存与 IRQ

```c
cursor = strstr(buf, "BOARD_PHYSMEM_LIST");
section_end = strchr(cursor, ']');
cursor = strchr(cursor, '(');
while (cursor && cursor < section_end && board->physmem_count < CFGCHK_MAX_PHYSMEM) {
    if (sscanf(cursor, "(%llx,%llx,%31[^)])", &start, &end, type_buf) == 3) {
        board->physmem[count].start = start;
        board->physmem[count].end = end;
        board->physmem[count].type = strstr(type, "Normal") ? CFGCHK_MEM_RAM : CFGCHK_MEM_IO;
        board->physmem_count++;
    }
    cursor = strchr(cursor + 1, '(');
}
```

- 顺序解析 `(start,end,type)` 元组，记录物理内存区间与类型。

```c
cursor = strstr(buf, "ROOT_ZONE_IRQS");
...
while (token && board->root_irq_count < CFGCHK_MAX_IRQS) {
    if (parse_hex_or_dec(token, &irq) == 0)
        board->root_irqs[board->root_irq_count++] = (uint32_t)irq;
    token = strtok(NULL, ",");
}
```

- 采集根区占用的 IRQ 编号。

#### 2.2.4 MPIDR 映射

```c
cursor = strstr(buf, "BOARD_MPIDR_MAPPINGS");
...
while (scan < list_end && idx < CFGCHK_MAX_CPUS) {
    ...
    if (parse_hex_or_dec(token, &mpidr) == 0)
        board->mpidr_map[idx++] = mpidr;
}
```

- 将 MPIDR 列表保存到 `board->mpidr_map`，供 DTS → CPU 映射使用。

### 2.3 解析 `zone1-linux.json`

核心片段：

```c
item = SAFE_CJSON_GET_OBJECT_ITEM(root, "cpus");
cnt = SAFE_CJSON_GET_ARRAY_SIZE(item);
for (i = 0; i < cnt; ++i) {
    zone->cpus[i] = cpu->valueint;
    zone->cpu_bitmap |= 1ULL << cpu->valueint;
}

item = SAFE_CJSON_GET_OBJECT_ITEM(root, "memory_regions");
for (i = 0; i < cnt; ++i) {
    dst->start = strtoull(...);
    dst->size = strtoull(...);
    if (strcmp(type_str, "virtio") == 0) {
        dst->type = CFGCHK_MEM_VIRTIO;
        zone->virtio[zone->virtio_count++] = { base, size };
    } else if (...) ...
}

item = SAFE_CJSON_GET_OBJECT_ITEM(root, "interrupts");
zone->irqs[i] = irq->valueint;
for (i = 0; i < zone->virtio_count && i < zone->irq_count; ++i)
    zone->virtio[i].irq = zone->irqs[i];

item = SAFE_CJSON_GET_OBJECT_ITEM(root, "arch_config");
parse_hex_string(gicd_base->valuestring, &zone->gicd_base);
...
```

每一步都检查数组大小、类型，并填充 `struct cfgchk_zone_summary`。

### 2.4 解析 Zone/Root DTS

#### 2.4.1 MPIDR → CPU

```c
static int mpidr_to_cpu(const struct cfgchk_board_info *board, unsigned long long mpidr, uint32_t *cpu_out) {
    if (board && board->total_cpus) {
        for (i = 0; i < board->total_cpus && i < CFGCHK_MAX_CPUS; ++i) {
            if (board->mpidr_map[i] == mpidr ||
                (board->mpidr_map[i] & 0xffffffffULL) == (mpidr & 0xffffffffULL)) {
                *cpu_out = i;
                return 0;
            }
        }
    }
    *cpu_out = (uint32_t)(mpidr & 0xff);
    return 0;
}
```

- 优先使用 `board->mpidr_map` 精确匹配；若没找到，退化为 MPIDR 的低 8 位。

```c
if (strncmp(trim, "cpu@", 4) == 0) { in_cpu = 1; continue; }
if (in_cpu && strstr(trim, "reg =")) {
    if (sscanf(start + 1, "%31[^>]", token) == 1 && parse_hex_or_dec(token, &mpidr) == 0) {
        uint32_t cpu; mpidr_to_cpu(board, mpidr, &cpu);
        dts->cpus[dts->cpu_count++] = cpu;
    }
    in_cpu = 0;
    continue;
}
```

- 解析 `cpu@...` 节点内的 `reg` 属性 → CPU ID。

#### 2.4.2 内存与 VirtIO

```c
if (strstr(trim, "memory {")) { in_memory = 1; continue; }
if (in_memory && strstr(trim, "reg =")) {
    if (parse_reg_cells(trim, &base, &size) == 0) {
        dts->mem_regions[dts->mem_count].start = base;
        dts->mem_regions[dts->mem_count].size = size;
        dts->mem_regions[dts->mem_count].type = CFGCHK_MEM_RAM;
        dts->mem_count++;
    }
    in_memory = 0;
    continue;
}
```

- 将 DTS 中声明的内存节点收集到 `dts_zone`。

```c
if (strncmp(trim, "virtio_mmio@", 12) == 0) { ... }
if (in_virtio >= 0 && strstr(trim, "reg =")) {
    parse_reg_cells → `VirtIO base/size`
}
if (in_virtio >= 0 && strstr(trim, "interrupts =")) {
    parse_interrupt_cells → `VirtIO irq`
    in_virtio = -1;
}
```

- VirtIO 节点的 `reg`/`interrupts` 依次解析。

#### 2.4.3 Root DTS

```c
if (strstr(trim, "reserved-memory {")) { in_reserved = 1; continue; }
if (in_reserved && strstr(trim, "reg =")) {
    parse_reg_cells → base/size;
    board->reserved_mem[...] = { start, size };
    dts_root->mem_regions[...] = { start, size, CFGCHK_MEM_RAM };
}
```

- 记录 reserved-memory，后续确保 guest 所需内存落在预留区内。

### 2.5 标记保留内存：`apply_reservation_flags`

```c
for each zone->mem_regions[i]:
    if (mem->type != CFGCHK_MEM_RAM) continue;
    for each board->reserved_mem[j]:
        if (mem 落在 reserved 范围内) {
            mem->flags |= CFGCHK_MEM_F_REQUIRES_RESERVATION;
            break;
        }
```

- 将需要 reserved-memory 覆盖的 RAM 区打标记。

### 2.6 汇总其它 Zone JSON：`build_cfg_request`

```c
dir = opendir(dir_path);
while ((ent = readdir(dir)) != NULL) {
    if (!strstr(name, ".json") || strcmp(filepath, target_json) == 0)
        continue;
    if (req->zone_count >= CFGCHK_MAX_ZONES)
        continue;
    if (parse_zone_json(filepath, &other, NULL, 0) != 0) {
        log_warn("跳过无法解析的 JSON: %s", filepath);
        continue;
    }
    apply_reservation_flags(board, &other);
    req->zones[req->zone_count++] = other;
}
```

- 方便检测 CPU/内存/IRQ 是否与其它 zone 配置冲突。

---

## 3. 请求结构：`include/cfgchk.h`

```c
struct cfgchk_board_info {
    __u32 total_cpus;
    __u32 reserved;
    __u64 root_cpu_bitmap;
    __u64 mpidr_map[CFGCHK_MAX_CPUS];
    __u32 root_irq_count;
    __u32 root_irqs[CFGCHK_MAX_IRQS];
    __u32 physmem_count;
    __u32 reserved_count;
    struct cfgchk_physmem_range physmem[CFGCHK_MAX_PHYSMEM];
    struct cfgchk_reserved_range reserved_mem[CFGCHK_MAX_RESERVED];
    __u64 gicd_base, gicd_size, gicr_base, gicr_size;
    __u32 gic_version;
};
```

- `mpidr_map`：新增字段，维持 JSON ↔ DTS CPU 映射一致。
- 其它结构（`cfgchk_zone_summary`、`cfgchk_dts_summary`）分别承载 zone 和 DTS 摘要数据。

---

## 4. 内核态校验：`driver/cfgchk.c`

### 4.1 ioctl 处理

```c
static long cfgchk_ioctl(..., unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case HVISOR_CFG_VALIDATE:
        req = kmalloc(sizeof(*req), GFP_KERNEL);
        if (copy_from_user(req, (void __user *)arg, sizeof(*req))) { ... }
        ret = cfgchk_validate_request(req);
        kfree(req);
        return ret;
    default:
        return -ENOIOCTLCMD;
    }
}
```

- 复制用户态请求，调用核心函数 `cfgchk_validate_request`。

### 4.2 顶层检查逻辑

```c
if (req->version != CFGCHK_IOCTL_VERSION) return -EINVAL;
if (!req->zone_count || req->zone_count > CFGCHK_MAX_ZONES) return -EINVAL;
if (!req->board.total_cpus || req->board.total_cpus > CFGCHK_MAX_CPUS) return -EINVAL;
...
for each zone:
    if (zone->cpu_count > CFGCHK_MAX_CPUS || ...) return -EINVAL;

ret = validate_cpu(...);   if (ret) return ret;
ret = validate_memory(...);if (ret) return ret;
ret = validate_irqs(...);  if (ret) return ret;
ret = validate_gic(...);   if (ret) return ret;
CFGCHK_INFO("zone %u validation success", target->zone_id);
```

- 先做边界与数量检查，再调用四个子校验函数。

### 4.3 CPU 校验：`validate_cpu`

```c
if (cpu >= board->total_cpus) return -EINVAL;
if (seen & BIT_ULL(cpu)) return -EINVAL;
...
if (target->cpu_bitmap & board->root_cpu_bitmap) return -EINVAL;
for (each other zone) if (bitmap 重叠) return -EINVAL;
if (dts->cpu_count != target->cpu_count) return -EINVAL;
for (each dts cpu) if (!(target->cpu_bitmap & BIT_ULL(cpu))) return -EINVAL;
```

- 确保 zone 使用的 CPU：  
  1. 满足板级总数；  
  2. 与根区和其它 zone 不冲突；  
  3. 与 DTS 一致。

### 4.4 内存校验：`validate_memory`

```c
if (!mem->size) return -EINVAL;
if (mem->type == CFGCHK_MEM_VIRTIO && (!IS_ALIGNED(start, 4K) || ...)) return -EINVAL;
for (each board physmem) {
    if (mem 类型与 physmem 不匹配) continue;
    if (range_within(mem, physmem)) { covered = true; break; }
}
if (!covered) return -EINVAL;

if (mem->flags & CFGCHK_MEM_F_REQUIRES_RESERVATION) {
    if (!reserved_contains(mem, board->reserved_mem[*])) return -EINVAL;
}

for (other zones) if (range_overlaps(mem, other_mem)) return -EINVAL;

if (dts_zone->mem_count) {
    for (each dts mem) if (!找到相同 start/size/type 的 JSON mem) return -EINVAL;
}

for (each board reserved range) if (root DTS 未覆盖该范围) return -EINVAL;
```

- RAM、IO、VirtIO 区域都必须落在板级允许的物理区间内，并遵守 reserved-memory 约束。
- 与其它 zone 内存不能冲突。
- DTS 声明的内存必须在 JSON 中出现。

### 4.5 IRQ/VirtIO：`validate_irqs`

```c
for (each irq in target) {
    if (重复) return -EINVAL;
    if (与root IRQ冲突) return -EINVAL;
    if (与其它 zone IRQ 冲突) return -EINVAL;
}

if (dts_zone->virtio_count != target->virtio_count) return -EINVAL;
for (i=0; i<virtio_count; ++i) {
    if (va->base/size != vd->base/size) return -EINVAL;
    if (va->irq != vd->irq) return -EINVAL;
    if (virtio irq 未包含在 JSON interrupts) return -EINVAL;
}

for (each dts virtio irq) if (不在 JSON interrupts) return -EINVAL;
```

- 防止 IRQ 重复或跨 zone 冲突。
- 确保 VirtIO 设备的 MMIO/IRQ 与 DTS 一致。

### 4.6 GIC 校验：`validate_gic`

```c
if (zone->gic_version != board->gic_version) return -EINVAL;
if (zone->gicd_base/size != board->gicd_base/size) return -EINVAL;
if (zone->gicr_base/size != board->gicr_base/size) return -EINVAL;
```

- 版本与基址必须严格匹配板级定义。

---

## 5. 错误处理与调试建议

1. **CLI 日志**  
   - `validate.c:917`：一旦 `ioctl` 返回 <0，CLI 会打印 `"内核校验失败..."` 并显示 `errno`。

2. **内核日志 (`dmesg`)**  
   - `cfgchk: ...` 前缀的 `pr_err` 信息提供具体原因，比如内存超界、CPU 冲突等。

3. **常见修复手段**
   - 确认 `board.rs`、`zone*.dts`、`zone*.json` 是否成对使用（同一平台）。
   - 校验前重新加载 `hvisor.ko`、`cfgchk.ko`，避免旧模块解析新结构。
   - 使用 `e2fsck -fy` 保证 rootfs 未受损（否则旧文件残留也会导致校验失败）。

---

## 6. 示例：报错定位到内存区间错误

假设 `dmesg` 显示：
```
cfgchk: zone 1 memory region 0 (0x50000000 size 0x30000000 type 0)
       not inside board physmem list
```

排查流程：
1. 检查 `board.rs` → `BOARD_PHYSMEM_LIST` 是否包含该区间并标记为 `MemoryType::Normal`。
2. 确认 JSON 中的 `memory_regions` 是否写错地址/大小。
3. 若 CLI/模块版本不匹配（例如新增 `mpidr_map` 后未重新安装模块），也会导致 `board->physmem_count` 读取错误，需重新 `insmod` 最新的 `.ko`。

---

## 7. 总结

配置校验流程遵循以下步骤：
1. **用户态解析**：`board.rs`、Zone/Root DTS、Zone JSON → `cfgchk_request`。
2. **内核态校验**：`cfgchk_validate_request` 调用 CPU/内存/IRQ/GIC 专项检查，返回成功或详细错误。
3. **日志反馈**：用户态打印 `errno`、内核态输出 `cfgchk:` 详细原因。

只要确保文件互相匹配、模块版本一致，就能在启动 guest 之前捕获资源冲突与配置错误，提升系统稳定性。

---

## 8. 开发思路与流程图

下面从工程化角度梳理“配置校验模块”的整体开发思路，便于后续迭代或移植到其他平台。文中流程图使用 ASCII 字符绘制，任何 Markdown 渲染器均可直接查看。

### 8.1 顶层开发流程

```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ 需求分析      │    │ 接口设计      │    │ 数据建模      │
│ - 易错点识别  │ => │ - 协议/结构体 │ => │ - cfgchk_*    │
│ - 输入输出    │    │ - ioctl 编号  │    │ - 字段定义    │
└─────┬────────┘    └─────┬────────┘    └─────┬────────┘
      │                     │                 │
      v                     v                 v
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ 用户态实现    │ => │ 内核模块实现  │ => │ 测试与调试    │
│ - 解析/打包  │    │ - validate_* │    │ - 正负样例    │
│ - 调用 ioctl │    │ - 日志输出   │    │ - dmesg 观察  │
└─────┬────────┘    └─────┬────────┘    └─────┬────────┘
      │                     │                 │
      v                     v                 v
       ┌──────────────┐    ┌──────────────┐
       │ 文档 & 指南   │ => │ 持续维护      │
       │ - 用法说明    │    │ - 版本兼容    │
       │ - 排错经验    │    │ - 平台适配    │
       └──────────────┘    └──────────────┘
```

#### 关键节点说明
- **需求分析**：梳理 guest 启动过程中最常见的配置失误（CPU 冲突、内存越界、VirtIO IRQ 不匹配等），确定校验覆盖面。
- **接口设计**：选定 `/dev/hvisor_cfgchk` + ioctl 的通信方式，规划用户态/内核态数据结构。
- **数据建模**：使用 `cfgchk_board_info` / `cfgchk_zone_summary` 等结构统一描述资源，确保字段可扩展。
- **实现阶段**：先做用户态解析与请求拼装，再实现内核态校验逻辑，最后打通整条链路。
- **测试与调试**：准备一组正/负样例（合法配置、冲突配置、缺失字段等），通过 CLI + `dmesg` 验证各项报错是否符合预期。
- **文档化**：编写 README/指南（如 `VALIDATION_GUIDE.md`），降低使用门槛。
- **持续维护**：随着板级参数或 Hypervisor 接口变化，及时更新结构体与解析逻辑。

### 8.2 用户态实现流程

```
命令解析
   │
   v
定位文件 ──────────────┐
 board.rs / zone.dts   │
 root.dts              │
                       v
解析 board.rs ──→ 解析 zone JSON ──→ 解析 zone DTS ──→ 解析 root DTS
 (NCPUs/PhysMem/IRQ)    (CPU/Memory/etc)               (reserved-memory)
                       │
                       v
设置 reserved 标记 (apply_reservation_flags)
                       │
                       v
构建 cfgchk_request (含其它 zone)
                       │
                       v
open("/dev/hvisor_cfgchk")
                       │
                       v
ioctl(HVISOR_CFG_VALIDATE)
               ┌───────────────┴───────────────┐
               │                               │
            返回 0                          返回错误
               │                               │
         输出 “[OK]”                显示 errno + 提示查看 dmesg
```

### 8.3 内核态实现流程

```
ioctl(HVISOR_CFG_VALIDATE)
        │
        v
copy_from_user(cfgchk_request)
        │
        v
初始合法性检查 (版本 / 数量 / 上限)
        │
        v
validate_cpu
        │
        v
validate_memory
        │
        v
validate_irqs
        │
        v
validate_gic
        │
   ┌─────┴─────┐
   │           │
 全部通过     某项失败
   │           │
pr_info + return 0   pr_err + return -EINVAL
```

### 8.4 设计要点与经验

1. **结构对齐**：`cfgchk_request` 在用户态和内核态共享，扩展字段时一定要同步更新头文件并重新编译 `.ko` 与 CLI。
2. **回退策略**：解析 `BOARD_NCPUS` 时提供 `BOARD_MPIDR_MAPPINGS` 回退，从而兼容宏/表达式写法。
3. **跨文件交叉校验**：DTS → JSON → board.rs 三方一致性是校验重点，任何一边的改动都应同步另两边。
4. **输入限制**：内核态对数组长度、计数器做严格检查，避免因用户态传值异常导致内核越界。
5. **错误指引**：在 `dmesg` 中写清楚哪个 zone、哪个资源出现冲突，方便开发者定位。
6. **测试矩阵**：准备如下场景的配置，用于回归：
   - 正常配置：应输出成功。
   - CPU 重复/越界。
   - 内存不在 board 列表内，或与其它 zone 重叠。
   - VirtIO IRQ 与 DTS 不一致。
   - reserved-memory 缺失。

通过上述思路，可循序渐进地实现并维护配置校验模块，确保在 guest 启动前就能捕获潜在问题。
```
