# 面向 hvisor/hvisor-tool 的配置校验内核模块调研报告

## 1. 背景与目标

- **现有体系**：`hvisor` 是 Rust 实现的 Type-1 虚拟机监控器，在 EL2 层负责 Zone 管理；`hvisor-tool` 提供 zone0 Linux 环境下的命令行工具与内核模块（`hvisor.ko`、`ivc.ko`）以与 Hypervisor 协同工作。
- **需求动因**：在启动 guest VM 前，需要对 JSON 配置文件与对应的 DTS/DTB 进行一致性与安全性校验，以降低资源冲突、缺失节点等运行时风险。
- **目标**：结合 `hvisor` 与 `hvisor-tool` 的实现，明确新增“配置校验内核模块”的技术路线、接口设计、开发步骤与测试要点，为后续实现提供详细指导。

## 2. 现有项目结构概览

- `hvisor-tool/driver/Makefile`：使用 Kbuild 方式交叉编译内核模块，依据 `ARCH` 选择工具链前缀 (`aarch64-none-linux-gnu-` 等)，通过 `make -C $(KDIR) M=$(pwd)` 构建 `hvisor.ko` 与 `ivc.ko`。
- `hvisor-tool/tools/`：用户态 CLI（`hvisor`）负责解析 JSON，映射镜像至物理内存，并通过 `/dev/hvisor` 执行 `ioctl` 与 `mmap`。
- `hvisor/src`：Hypervisor 侧通过 `HyperCall` 接口处理 `HVISOR_HC_START_ZONE` 等命令，并维护 `CONFIG_MAGIC_VERSION` 确保 `zone_config_t` 结构一致。
- 核心构建命令：`make all ARCH=<arch> KDIR=~/linux` 会生成 `tools/hvisor`、`driver/hvisor.ko`、`driver/ivc.ko` 并复制到 `output/`。

## 3. 现有内核模块功能总结

### 3.1 hvisor.ko

- 注册 `/dev/hvisor` misc 设备，处理 `HVISOR_INIT_VIRTIO`、`HVISOR_ZONE_START`、`HVISOR_ZONE_LIST`、`HVISOR_CONFIG_CHECK` 等 `ioctl`。
- 通过 `copy_from_user` 获取 `zone_config_t`，再调用 EL2 Hypercall 启动 Zone，并支持 `mmap` 共享 Virtio 区。
- 依赖 DT 中的 `/hvisor_virtio_device` 节点获取中断号，若缺失则模块初始化失败。

### 3.2 ivc.ko

- ARM64 下从 Hypervisor 获取 IVC 共享内存信息，动态创建设备 `/dev/hivcX`。
- 提供 `mmap` 与 `poll` 支持，允许用户态映射控制表与共享内存；依赖 `/hvisor_ivc_device` 节点注册中断。

## 4. 用户态对配置文件的处理流程

- `tools/hvisor.c` 使用 `cJSON`（封装于 `safe_cjson.c`）解析 `zone_config` JSON，检查关键字段并转换为 `zone_config_t`。
- CLI 负责将 kernel/dtb 镜像通过 `/dev/mem` 加载到指定物理地址，再调用 `/dev/hvisor` 的 `HVISOR_ZONE_START`。
- 目前 JSON 合法性校验集中在用户态，DTS/DTB 是否与 JSON 匹配需要人工确认，存在遗漏风险。

## 5. 新增配置校验模块的需求分析

- **校验范围**：
  1. JSON 中的内存区、CPU、IRQ、Virtio/IVC 配置是否合规，数值范围、对齐、重复项等。
  2. DTS/DTB 中是否存在 JSON 声明的节点、地址范围、中断号，确保资源不会缺失或冲突。
- **定位**：该模块应在 zone0 Linux 中运行，既可作为启动前的“守门人”，也可嵌入 CI/部署流程。
- **约束**：内核态缺乏成熟的 JSON 解析库，需控制输入尺寸与安全性；同时保持与现有 `hvisor.ko` 的接口兼容。

## 6. 技术可行性与选择

- **编程语言**：沿用 C，复用 `driver/Makefile` 的 Kbuild 流程，确保交叉编译链一致。
- **JSON 处理方案**：
  - 推荐在用户态先解析 JSON，将结构化数据（`zone_config_t` + 附加元信息）传入内核模块；内核侧专注于“二次校验”。
  - 如需内核直接解析，可自研简单解析器或移植轻量 JSON 库（需严格审计），但维护成本较高。
- **DTS/DTB 校验**：利用 Linux OF API（`of_find_node_by_path`、`of_property_read_*`、`of_address_to_resource` 等）与运行中 DT 对比，验证节点/资源是否存在及范围匹配。
- **Hypervisor 交互**：可复用 `hvisor_call`，在模块加载或校验前调用 `HVISOR_CONFIG_CHECK` 确保结构体版本一致。

## 7. 模块设计方案

### 7.1 整体架构

```
用户态 CLI (hvisor zone validate)
    ↓ 解析 JSON & DTS → 生成结构化校验请求
配置校验内核模块 (/dev/hvisor_cfgchk)
    ↓ 校验 JSON 结构 → 校验 DTS/系统资源 → 返回结果/日志
必要时调用 hvisor.ko 接口或 Hypercall 获取追加信息
```

### 7.2 内核模块接口建议

- 注册 `misc` 设备 `/dev/hvisor_cfgchk`。
- 定义 `ioctl`：
  - `CFGCHK_VALIDATE_ZONESCHEMA`：参数为用户态填充的 `cfgchk_request`，包含 `zone_config_t` 与附加校验标志。
  - `CFGCHK_VALIDATE_DTB`（可选）：接受 DTB blob 的物理地址或句柄，用于离线校验。
- `ioctl` 返回值用于区分成功/失败；失败时通过 `pr_err` 打印详细原因，并在用户态回传错误码与文本描述。

### 7.3 校验核心逻辑

- **结构校验**：
  - 检查数组长度与 `CONFIG_MAX_*` 限制一致；CPU 位图是否与 `ARCH` 支持的核数匹配。
  - 内存区是否 4KB 对齐、是否存在重叠；`kernel_load_paddr`、`dtb_load_paddr` 是否落在已声明 RAM 范围内。
  - IRQ 是否落在合法区间且不重复；PCI 相关配置的地址区间是否合理。
- **DTS 校验**：
  - 通过 `of_find_compatible_node` 或 `of_find_node_by_path` 检查 JSON 中列出的设备节点。
  - 对比 `interrupts`、`reg` 属性，与 JSON 中的地址/IRQ 做一一匹配。
  - 可扩展：验证 Virtio/IVC 所需节点（`/hvisor_virtio_device`、`/hvisor_ivc_device`）及中断数量。
- **版本校验**：调用 `HVISOR_CONFIG_CHECK`，若内核侧 `CONFIG_MAGIC_VERSION` 与 Hypervisor 不一致，应拒绝校验并提示升级。

### 7.4 用户态集成

- 在 `tools/hvisor` 中新增子命令 `zone validate <config.json>`：
  1. 解析 JSON → 生成 `zone_config_t`。
  2. 可选解析预生成的 DTB 或读取运行内核的 `/sys/firmware/devicetree`。
  3. 将数据通过 `/dev/hvisor_cfgchk` 提交校验，输出结果并决定是否继续执行 `zone start`。
- 在 README 或 `AGENTS.md` 中补充使用说明，指导开发者在提交前执行校验。

## 8. 实施步骤（建议）

1. **准备环境**：确认目标板的内核源码路径（`KDIR`）、交叉编译链（如 `aarch64-none-linux-gnu-`）。
2. **扩展 driver/Makefile**：在 `obj-m` 中新增 `cfgchk.o`，并在顶层 `Makefile` 的 `driver` 规则中复制新生成的 `.ko`。
3. **定义公共头文件**：在 `include/` 下新增 `cfgchk.h` 定义 `ioctl` 编号与请求结构，复用 `zone_config.h` 常量。
4. **实现内核模块主体**：
   - `module_init` 中校验 Hypervisor 版本，初始化所需资源。
   - `ioctl` 分支完成 JSON 结构、DTS 节点、资源范围等检查。
   - 利用 `of_*` API 访问当前设备树，必要时解析传入的外部 DTB。
5. **用户态 CLI 更新**：添加 `zone validate`，封装与 `/dev/hvisor_cfgchk` 的交互，并对校验结果进行友好提示。
6. **测试验证**：
   - 在 QEMU 或实际硬件上加载 `cfgchk.ko`，使用正确/错误的 JSON、DTS 组合测试；
   - 覆盖内存重叠、IRQ 缺失、节点不存在等场景，确保能准确定位问题。
   - 将测试步骤整合进项目 CI（可仿照 `.github/workflows/ci.yml` 增加新 job）。
7. **文档与培训**：更新 README、贡献者指南，记录校验流程与常见错误提示；必要时整理示例（`examples/` 下新增 `*_invalid.json`、`README` 说明）。

## 9. 风险与注意事项

- 内核态解析数据需谨慎处理边界条件，限制输入长度，避免内核 panic；推荐先在用户态完成解析后再传结构体。
- 若后续 Hypervisor 更新 `zone_config` 字段或 `CONFIG_MAGIC_VERSION`，需同步更新校验逻辑；可在 CI 中增加“版本漂移检测”。
- DTS 校验依赖运行时设备树，若用户希望离线校验原始 `.dts`，需额外实现 DTB 解析流程或保持在用户态完成。
- 模块应保持与现有 `hvisor.ko`、`ivc.ko` 并存，不干扰原有 `ioctl`；发生异常时仅返回错误与日志，不修改系统状态。

## 10. 参考资料与后续建议

- `hvisor-tool` 仓库：`driver/hvisor.c`、`driver/ivc_driver.c`、`tools/hvisor.c`、`include/zone_config.h`。
- `hvisor` 仓库：`src/config.rs`、`src/arch/*/hypercall.rs`、`platform/<arch>/<board>/` 示例配置。
- Linux 设备树 API：`Documentation/devicetree/usage-model.rst`、`drivers/of/` 示例实现。
- 建议先在用户态模拟校验流程（例如 Python/Go 脚本），明确规则后再迁移至内核模块，降低首版实现复杂度。
