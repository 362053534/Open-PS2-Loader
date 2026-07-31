# PCSX2 与 OPL SMB2 联动测试

## 结论

PCSX2 可以作为 OPL SMB2 的快速联动测试环境。它能够通过 DEV9 访问局域网中的 SMB 服务器，并能在黑屏时从外部暂停虚拟机，分别检查 EE（R5900）和 IOP（R3000），不依赖游戏内 IGR。

模拟器适合定位协议错误、短读取、无效句柄、IOP 卡死位置和内存破坏；网络吞吐量、线程时序与真实 DEV9 硬件仍须在实机复核。

## 推荐环境

1. 使用当前 PCSX2 Nightly 的便携版，并使用用户从自己主机提取的 BIOS。
2. 为 OPL 单独建立一张虚拟记忆卡，保存网络和 SMB 配置。
3. 在 `设置 -> 网络与硬盘` 中启用以太网。
4. Windows 下优先使用 `PCAP Bridged` 并选择实际有线网卡，使 OPL 的 PS2IP/SMB2 链路尽量接近实机。`Socket` 后端适合先验证能否连接，但应作为第二组对照，因为它会改变网络后端行为。
5. OPL 中直接填写 SMB 服务器 IP，避免把名称解析问题混入测试。

PCSX2 官方记录确认 DEV9 提供 Socket 后端，并保留 TAP/PCAP 以连接局域网和真实硬件：[PCSX2 Q1 2022 Progress Report](https://pcsx2.net/blog/2023/q1-2022-progress-report/)。Windows 的 PCAP Bridged/Switched 支持也有官方更新记录：[PCSX2 Q1 2021 Progress Report](https://pcsx2.net/blog/2021/q1-2021-progress-report/)。

## 启动 OPL

首次测试使用 GUI，便于同时打开调试器。配置完成后，可用命令行重复启动：

```text
pcsx2-qt.exe -batch -elf "C:\GitHub\Open-PS2-Loader\OPNPS2LD.ELF"
```

不同版本的可执行文件名可能不同，应先执行 `pcsx2-qt.exe -help` 核对参数。PCSX2 的 Qt 命令行支持 `-batch`、`-elf`、`-fastboot` 等参数：[PCSX2 Command-line support](https://wiki.pcsx2.net/index.php?title=Command-line_support)。

## 黑屏定位

1. 在 PCSX2 中启用 `工具 -> 显示高级设置`，打开 `调试 -> 打开调试器`。
2. 从 OPL 启动同一个 SMB 游戏，等待出现 PS2 Logo 后黑屏。
3. 直接在 PCSX2 中暂停虚拟机，不需要 IGR。
4. 查看 R5900：确认游戏入口是否已经执行，以及当前 EE 线程是否卡在 SIF RPC 等待。
5. 查看 R3000：确认 IOP 当前 PC 是否位于 `smb_ReadFile()`、`smb2_pread()`、socket/select 或 `cdvdfsv` RPC 处理路径。
6. 使用调试版 `smb_cdvdman.irx` 的 map/symbol 信息，将模块实际加载基址加上函数偏移，导入为 PCSX2 `.sym` 符号。
7. 在 `smb2_pread()` 返回位置设置断点，记录请求长度、返回长度、文件偏移和错误值。若返回值为正但小于请求长度，应观察续读是否从正确偏移继续。

PCSX2 官方调试器提供独立的 R5900 与 R3000 布局、断点、寄存器、内存查看和 ELF/外部符号导入：[PCSX2 Debugger](https://pcsx2.net/docs/advanced/debugger/)。当前版本可读取 ELF `.symtab`、MIPS `.mdebug` 和外部 `.sym` 文件：[PCSX2 2.2/2.4 Debugger and Symbol Parsing](https://pcsx2.net/blog/2025/pcsx2-2.4_2.2/)。

## 网络抓包

在 SMB 服务器或 PCSX2 所使用的网卡上用 Wireshark 捕获：

```text
tcp.port == 445
```

重点对照：

- 最后一条 SMB2 READ 请求和响应的 MessageId；
- 请求长度与响应 DataLength；
- 是否出现 TCP RST、重传或长时间无响应；
- SMB2 Status、Dialect、Credits 和 MaxReadSize；
- 黑屏前最后一次成功读取的文件偏移。

PCAP 后端直接经过选定网卡，适合抓取接近 PS2 原始链路的数据。Socket 后端的抓包更容易，但其主机侧行为不能代替实机时序验证。

## 建议的两级反馈循环

### 第一级：完整 OPL 启动

每次构建后命令行启动 OPL，进入固定 SMB 游戏；黑屏时暂停 PCSX2，同时保存 R5900、R3000 状态和 PCAP。这一级验证真实 OPL 模块装载与游戏启动链路。

### 第二级：SMB2 读取测试 ELF

后续可建立一个小型 PS2 ELF，只加载与 OPL 相同的 SMAP、PS2IP 和 SMB2 代码，对同一个 ISO 执行固定偏移、固定长度的读取并校验散列。它能把一次测试从完整启动缩短到数秒，并稳定复现短读、跨边界读取和连接中断。

PCSX2 官方过去也采用“同一测试程序分别在实机和模拟器运行并比较日志”的差分方法：[PCSX2 Auto test suite](https://pcsx2.net/blog/2016/january-february-2016-progress-report/)。

## 不能由模拟器最终证明的内容

- 真实 DEV9/SMAP DMA、总线争用和 IOP 时序；
- 实机网络端口的吞吐量上限；
- 机械硬盘、路由器或 NAS 导致的延迟抖动；
- 仅在实机线程调度和缓存一致性下出现的竞态。

因此正确流程是：PCSX2 快速定位和缩小范围，最终候选修复仍在同一台 PS2、同一服务器、同一镜像上验证。
