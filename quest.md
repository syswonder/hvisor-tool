你可以参考的内核源码： ~/workspace/kernel/
你需要修改的设备树：zone1-linux.dts

你的任务：上周碰到了这个问题：
```
    plat->stmmac_rst = devm_reset_control_get(&pdev->dev,
                          STMMAC_RESOURCE_NAME);
    if (IS_ERR(plat->stmmac_rst)) {
        if (PTR_ERR(plat->stmmac_rst) == -EPROBE_DEFER)
            goto error_hw_init;

        dev_info(&pdev->dev, "no reset control found\n");
        plat->stmmac_rst = NULL;
    }
```
这里我们没有正确实现scmi的reset协议，所以这里获取reset控制器资源失败。我们需要修改zone1linux（guest）的设备树，并且增加scmi-reset协议。协议内容可在scmi_reset.md中查看。


参考资料：
2025.1.8
上周遇到了reset-controller没有正常找到的问题，本周先研究scmi有没有reset-controller相关代码。
什么是reset domain？
查看scmi protocol，发现只有reset domain protocol是和复位信号相关的。

想象一个硬件里面有很多硬件模块，比如 CPU、GPU、网络模块等。reset domain 就像把其中一部分硬件模块捆成一组，可以一次性“重启”它们。
一组设备：一个 reset domain 不是单独的设备，而是一组可以一起复位的设备。
复位方式：
- 自主复位（Autonomous）：硬件自己完成复位，你只告诉它“去复位”。
- 显式复位（Explicit）：你需要手动先发“开始复位”信号，然后再发“结束复位”信号。

Scmi-reset 机制
SCMI（System Control and Management Interface）规范里对 reset domain 做了系统化管理，它是硬件复位的“接口层”。理解起来，可以分几个点：
1. 一个 reset domain 就是一个可被 SCMI 控制复位的设备组。有一个 唯一的 domain_id（从 0 开始的整数），SCMI 命令通过这个 ID 指定要复位的域。
2. SCMI 提供了一组命令来管理 reset domain：
  - PROTOCOL_VERSION / NEGOTIATE_PROTOCOL_VERSION：确认 SCMI 协议版本。
  - RESET_DOMAIN_ATTRIBUTES：查看 reset domain 的属性，比如能否异步复位、复位延迟、名称等。
  - RESET：执行复位（可自主或显式、同步或异步）。
  - RESET_NOTIFY：当域被复位时，平台通知调用方。
  - RESET_DOMAIN_NAME_GET：获取 reset domain 的完整名称（尤其是超过 16 字节的情况）。

RK3588上的 reset 可以映射到 scmi-reset 吗？
答案是肯定的！
在 RK3588 这种 SoC 上，CRU 是一个集成的硬件模块，它管理：
- 时钟（Clock）：为各个模块提供不同频率的时钟信号
- 复位（Reset）：为各个模块提供复位信号
CRU 本质上就是一个“统一的时钟和复位控制器”。在设备树里，你会看到类似 <&cru_phandle sig_id> 的引用，这里 sig_id 可以指一个时钟信号或者复位信号。

而SCMI 提供统一接口给操作系统或管理固件操作clock和reset domain：
- scmi-clock
  - 映射到 CRU 的时钟
  - 软件通过 SCMI 命令控制时钟频率、开关等
- scmi-reset
  - 映射到 CRU 的复位信号
  - 软件通过 SCMI 命令触发 reset domain
也就是说，SCMI 并不关心底层 CRU 的实现细节，它只要能把 reset domain 对应到 CRU 的某个复位信号，就可以控制复位。scmi-reset 驱动通过 domain_id ↔ CRU reset signal 建立映射，操作系统通过 SCMI 协议发 reset 命令 → SCMI driver → CRU → 触发硬件复位。
因此，就像 scmi-clock 映射到 CRU 时钟一样，scmi-reset 可以映射到 CRU 复位信号。

添加scmi-reset的设备树节点
protocol@16 {    // reset protocol
    reg = <0x16>;
    #reset-cells = <0x01>;
    phandle = <0x1002>;  // 新的phandle值
};


