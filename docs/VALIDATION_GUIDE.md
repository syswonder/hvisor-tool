# hvisor Guest 配置校验使用指南

本文说明如何在 `hvisor-tool` 中使用新增的 `cfgchk` 内核模块与 `hvisor zone validate` 指令，对 guest VM 的 JSON/DTS 配置进行一致性校验。

## 1. 构建

1. 准备好目标板的交叉编译链与 `KDIR`（已执行 `make modules_prepare` 的 Linux 内核源码树）。
2. 在 `hvisor-tool` 根目录执行：
   ```bash
   make driver ARCH=<arch> KDIR=/path/to/linux
   ```
   编译完成后，`output/` 目录会生成：
   - `hvisor.ko`
   - `ivc.ko`（如适用）
   - `cfgchk.ko`

## 2. 部署与加载

1. 将上述 `.ko` 文件拷贝到 zone0 Linux 管理机。
2. 加载内核模块（只需一次）：
   ```bash
   sudo insmod hvisor.ko
   sudo insmod cfgchk.ko
   ```
   成功后系统会创建 `/dev/hvisor` 与 `/dev/hvisor_cfgchk`。

## 3. 目录要求

- 在 `hvisor` 仓库中，每个平台的配置布局如下：
  ```
  platform/<arch>/<board>/
    ├── board.rs
    ├── configs/*.json
    └── image/
         ├── zone0.dts
         └── <zone-name>.dts
  ```
- `hvisor zone validate` 默认假定：
  - JSON 位于 `configs/`
  - 同名 DTS 位于 `image/`
  - `board.rs` 位于上级目录
  - `zone0.dts` 提供 root zone 的 reserved-memory 描述

## 4. 执行校验

在目标 JSON 所在目录执行：
```bash
cd platform/aarch64/rk3588/configs
hvisor zone validate zone1-linux.json
```

流程说明：
1. 工具解析 `board.rs`、`zone0.dts`、目标 `zone1-linux.json` 与对应 `zone1-linux.dts`。
2. 自动收集同目录下其余 `.json`，检查 CPU、内存、中断等资源是否冲突。
3. 通过 `/dev/hvisor_cfgchk` 调用内核态校验逻辑，核对以下项目：
   - CPU 是否超出总核数、与 root zone 或其他 guest 冲突，并与 DTS 对齐。
   - RAM 区域是否位于合法物理内存段、必要时落在 reserved-memory 内。
   - VirtIO MMIO 范围、大小、IRQ 是否与 DTS 一致，且不与 root IO 段冲突。
   - 中断号与 root/其他 guest 去重。
   - GIC 版本与基地址匹配。

若校验通过：
```
[OK] Zone 1 资源校验通过。
```
若失败，请查看终端输出及 `dmesg`，内核会指出具体冲突条目（例如 VirtIO IRQ 对应不上或内存越界）。

## 5. 常见问题

- **提示找不到 `/dev/hvisor_cfgchk`**：确认 `cfgchk.ko` 已正确加载。
- **解析失败**：确保 JSON/DTS 文件遵循 hvisor 示例格式（双 cell 表示 64 位地址、设备节点名称 `virtio_mmio@...` 等）。
- **目录结构不同**：如有自定义布局，可在加载前手动调整路径或扩展工具逻辑。

## 6. 建议的使用节奏

1. 每次修改 JSON/DTS 后，先运行 `hvisor zone validate ...`。
2. 校验通过再执行 `hvisor zone start ...` 正式启动 guest。
3. 将校验命令纳入 CI 或提交流程，避免资源冲突问题在运行时才暴露。
