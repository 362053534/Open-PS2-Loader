# SMB 空闲 Echo 改动与排查记录

基线：`1690253da723a58aa12c16eb82862a56c3fdf6bb`。

## 当前结论

源码改动和主机回归验证已完成；PS2 debug ELF 编译、PCSX2 启动、实机长时间挂机与拔线 IGR **未验证**。不能据此宣布现场断流或整机冻结已经修复。

用户确认的测试边界是实际 Echo 收发、空闲调度与读盘交接；超时策略是保留连接，并安全处理迟到回包。

## 最终行为

- 连续空闲约一分钟后发送真正的 SMB1 `SMB_COM_ECHO`，正常空闲时每分钟一次。实际读盘成功后重新计算空闲周期；读盘优先。
- 一次只请求一个 Echo 回包，核对 MID、响应标志、状态、序号、长度和回显载荷。
- 整包共用一个六秒截止时间；不会因 NBSS 头、正文分段或收到旧 Echo 而重新计时。只有本次请求已入队且等待满六秒，才累计一次未回包。
- 忙锁、发送失败、对端关闭及协议错误不记为未回包。发送队列满时立即返回，不进入阻塞写入，也不覆盖连接原来的错误状态。
- Echo 超时不主动断线、不重连、不模拟开仓。控制包未完整到达时保留在 socket 内；迟到 Echo 在后续 Echo、读盘和控制收包路径中被识别并跳过。
- 移除 SMB 专用 TCP 保活调用、导出和消息，以及无其他调用者的 `tcp_keepalive()` 构包实现。
- 移除空闲 PHY 检测和空闲开关仓检查。真实读盘卡住时的链路检查保留；因此空闲期间不再通过周期 PHY 检查恢复模拟关仓。
- 周期闹钟只排队，不做网络 I/O；由同一只闹钟持续续期。Echo 在读线程执行，停止设备时取消闹钟。

`__IOPCORE_DEBUG` 下使用 `[SMBECHO]` 日志：`sent`、`result`、`timeout ... missed=N`。编译时需启用 `IOPCORE_DEBUG=1`；单独 `make debug` 不保证启用这些 IOP 日志。

## 排查所得

### 1. 旧 TCP 保活的标志位未初始化，已用实际函数重现

旧 `tcp_keepalive()` 分配 `PBUF_RAM` 后没有初始化 `_hdrlen_rsvd_flags`，直接调用 `TCPH_HDRLEN_SET()`。这个宏会保留原先的 TCP flags；`PBUF_RAM` 的分配也不会清空载荷。

`python3 tests/smb/probe_legacy_tcp.py` 从固定基线提取原函数、原 TCP 头和原宏，只替换分配及网络输出边界，得到：

```text
payload fill=0x00 -> TCP flags=0x00 (ACK absent)
payload fill=0xff -> TCP flags=0x3f (FIN|SYN|RST|PSH|ACK|URG)
```

这证实旧代码会把旧内存中的标志带进发包，是明确的报文构造缺陷。它可能破坏 TCP/SMB 会话，但没有抓取现场报文，不能确认它就是用户现场断流的唯一原因；也不能据此认定发生的是物理端口掉线。

### 2. 恢复 Echo 时必须解决半包超时和迟到回包

直接恢复旧分阶段收包方法时，已复现：NBSS 头仅收到两个字节便超时，后续读盘从余下两个字节开始解析，导致失败。测试使用实际 `smb.c`、`device-smb.c` 和实际 `lwip_recvfrom()`，不是独立重写的协议模型。

最终控制包接收在整包就绪前不推进 socket 的消费位置；超时后也不登记一个必须永久等待的“旧 Echo 包”。下一笔读可以先发送，并在接收时跳过迟到 Echo。已覆盖头部及正文多个截断位置、粘包、旧 MID，以及 Echo 永远不回但下一笔读正常返回的情况。

### 3. 仅限制收包不能避免发送阶段占住锁

旧 `lwip_send()` 忽略发送 flags，TCP 总是进入 `netconn_write()`。发送空间不足时，旧实现可以一直等信号量。现已增加核心锁内整包尝试入队的非阻塞路径，供 Echo 使用；普通读写的发送语义维持原状。

### 4. IGR 现象的代码依据与尚未确认的部分

`oplShutdown()` 先调用 `DeviceLock()`，后者等待 `smb_io_sema`。数据读、VMC 和 Echo 都与此锁有关。因此“断流后 IGR 反而可用”不能单独证明物理连接状态，也不能证明此前锁未被占住；网络错误让 I/O 提前返回时，锁的表现可能不同。

本次确保 Echo 不等待忙锁、发送不等 TCP 队列空间、等待回包有六秒总限，且每条出口释放已取得的锁。**没有证明 IGR 的全部路径均有超时**：普通数据读、Close 的发送和硬件驱动等待仍有既有的阻塞行为。原整机冻结未复现，不能把这些风险写成已确认根因。

## TDD 与测试审计

命令：

```sh
python3 tests/smb/run.py
python3 tests/smb/run.py --debug
python3 tests/smb/probe_legacy_tcp.py
python3 tests/smb/mutation_probe.py
```

主机测试启用 AddressSanitizer、UndefinedBehaviorSanitizer；SMB 联测遇到 sanitizer 错误立即失败。共 27 个执行场景，包括 9 个额外的半包截断位置及实际 lwIP 非阻塞发送入口测试。

| 用户可判断的规则 | 测试场景 | 结果 |
| --- | --- | --- |
| 发出真正的 SMB Echo 并消费回包，不发 TCP 探测 | echo、send_nonblocking | PASS |
| 成功读盘后进入空闲，每分钟发一次 Echo，不查 PHY | idle | PASS |
| 未回包恰好等待六秒，且只记录一次 | timeout、debug 日志断言 | PASS |
| 超时不把下一分钟延长成 66 秒 | timeout_cadence | PASS |
| 5.98 秒到达的完整 Echo 仍可成功 | delayed_success | PASS |
| 系统时钟低位回绕不破坏六秒截止时间 | clock_wrap | PASS |
| NBSS 头及正文半包超时后不破坏下一笔读盘 | late_partial 与额外 9 个截断位置 | PASS |
| 旧 MID 的 Echo 不能被当成本次成功 | wrong_mid | PASS |
| 对端错误、坏回包、关闭连接不算超时未回包 | server_error、bad_echo、closed | PASS |
| Echo 不等待 VMC/IGR 已持有的锁 | lock_busy | PASS |
| 发送失败不持锁等待，也不累计未回包 | send_blocked | PASS |
| 已排队的真实读优先，空闲唤醒不重复读上一笔 LBA | read_priority，调用设备/调度入口 | PASS；完整读线程和游戏回调需上机确认 |
| Echo 永远不回时，正常的下一笔读不能卡在旧包清理中 | read_after_missing | PASS |
| 停止设备后不再发 Echo | shutdown | PASS |
| 队列不足时整包失败，恢复后仍可发送，不污染连接错误；拒绝截断长度 | 实际 lwip_send、netconn_trywrite、do_trywrite 联测 | PASS |

这些句子是测试所保护的行为规则；若其中有与预期不符的规则，应先修正规则，再修改测试。

测试的限制：PS2 内核、时钟、TCP 缓冲和硬件边界使用受控替身；实际 SMB 文件整段编译，lwIP 收发相关函数及消息结构从生产源码原样提取。仅对 Linux 没有的 `sockaddr_in.sin_len` 字段做平台适配。它们不替代 PS2 完整链接、真实多线程抢占、真实路由器、SMAP DMA 或长时间挂机验证。

隔离副本中实施 10 个定向变异，全部被测试拒绝；没有将变异写入工作区，运行前后校验生产文件哈希一致。变异涵盖五秒超时、六秒边界、提前发送、恢复空闲 PHY 查询、不跳过迟到 Echo、不核对 MID、等待忙锁、阻塞发送、发送容量等号边界和长度截断。该探针不是穷尽证明。

## 上机验证状态

已通过用户提供的 runner 配置连接远程 Windows。PATH 和常见默认安装位置均未发现 Docker/PCSX2，默认 BIOS 路径也未发现文件；这不是对整台机器所有目录的搜索。没有安装组件、修改系统配置或启动游戏。

| 验收项 | 结论 | 证据或缺少条件 |
| --- | --- | --- |
| 主机协议及调度回归 | PASS | host-tests.log、host-tests-debug.log |
| 旧 TCP flags 缺陷复现 | PASS | legacy-tcp-probe.log |
| 关键规则变异探针 | PASS | mutation-probe.log |
| 云电脑 Docker 编译 PS2 debug ELF | 未验证 | 需要可用 Docker、`ghcr.io/362053534/ps2homebrew` 最新镜像及完整 PS2 工具链 |
| PCSX2 启动与截图、emulog | 未验证 | 需要 PCSX2、用户 BIOS、测试介质和 SMB 服务 |
| 原断流、原整机冻结、拔线 IGR | 未验证 | 需要原游戏和原网络环境长时间复现 |

仓库未提供 `docs/agents/feedback-loops.md` 或本任务的应用启动反馈环路；后续可按 `/setup-feedback-loops` 补齐环境入口。未把主机测试通过记为上机通过，也未进行性能 A/B 对比。

后续上机应在 `/workspace/opl-debug/` 保存 debug ELF、截图、emulog 和 results.md；启用 `[SMBECHO]` 日志，分别记录正常挂机、丢 Echo 回包、空闲恢复读盘和拔线 IGR。若再冻结，应先区分物理 link、TCP 会话与 IOP/EE 活性，避免把三种现象统称为断流。
